#include "moveGen.hpp"

#include "dynamicConfig.hpp"
#include "logging.hpp"
#include "material.hpp"
#include "positionTools.hpp"
#include "searcher.hpp"
#include "tools.hpp"

#ifdef WITH_NNUE
#include "nnue.hpp"
#include "nnue/nnue_accumulator.h"
#endif

namespace MoveGen{

void addMove(Square from, Square to, MType type, MoveList & moves) {
  assert(from >= 0 && from < NbSquare);
  assert(to >= 0 && to < NbSquare);
  moves.push_back(ToMove(from, to, type, 0));
}

} // MoveGen

namespace{
    std::array<CastlingRights,NbSquare> castlePermHashTable;
    std::mutex castlePermHashTableMutex;
} 

void initCaslingPermHashTable(const Position & p){
    // works also for FRC !
    const std::lock_guard<std::mutex> lock(castlePermHashTableMutex); // this function must be thread safe !!!
    castlePermHashTable.fill(C_all);
    castlePermHashTable[p.rooksInit[Co_White][CT_OOO]]  = C_all_but_wqs;
    castlePermHashTable[p.kingInit[Co_White]]           = C_all_but_w;
    castlePermHashTable[p.rooksInit[Co_White][CT_OO]]   = C_all_but_wks;
    castlePermHashTable[p.rooksInit[Co_Black][CT_OOO]]  = C_all_but_bqs;
    castlePermHashTable[p.kingInit[Co_Black]]           = C_all_but_b;
    castlePermHashTable[p.rooksInit[Co_Black][CT_OO]]   = C_all_but_bks;
}

namespace{
    // piece that have influence on pawn hash
    const bool _helperPawnHash[NbPiece] = { true,false,false,false,false,true,false,true,false,false,false,false,true };
}

void movePiece(Position & p, Square from, Square to, Piece fromP, Piece toP, bool isCapture, Piece prom) {
    START_TIMER
    const int fromId   = fromP + PieceShift;
    const int toId     = toP + PieceShift;
    const Piece toPnew = prom != P_none ? prom : fromP;
    const int toIdnew  = prom != P_none ? (prom + PieceShift) : fromId;
    assert(squareOK(from));
    assert(squareOK(to));
    assert(pieceValid(fromP));
    // update board
    p.board(from) = P_none;
    p.board(to)   = toPnew;
    // update bitboard
    BBTools::unSetBit(p, from, fromP);
    _unSetBit(p.allPieces[fromP>0?Co_White:Co_Black],from);
    if (toP != P_none){
        BBTools::unSetBit(p, to,   toP);
        _unSetBit(p.allPieces[toP>0?Co_White:Co_Black],to);
    }
    BBTools::setBit  (p, to,   toPnew);
    _setBit(p.allPieces[fromP>0?Co_White:Co_Black],to);
    // update Zobrist hash
    p.h ^= Zobrist::ZT[from][fromId]; // remove fromP at from
    p.h ^= Zobrist::ZT[to][toIdnew]; // add fromP (or prom) at to
    if (_helperPawnHash[fromId] ){
       p.ph ^= Zobrist::ZT[from][fromId]; // remove fromP at from
       if ( prom == P_none) p.ph ^= Zobrist::ZT[to][toIdnew]; // add fromP (if not prom) at to
    }
    if (isCapture) { // if capture remove toP at to
        p.h ^= Zobrist::ZT[to][toId];
        if ( (abs(toP) == P_wp || abs(toP) == P_wk) ) p.ph ^= Zobrist::ZT[to][toId];
    }
    // consequences on castling 
    if (p.castling && castlePermHashTable[from] ^ castlePermHashTable[to]){
       p.h ^= Zobrist::ZTCastling[p.castling];
       p.castling &= castlePermHashTable[from] & castlePermHashTable[to];
       p.h ^= Zobrist::ZTCastling[p.castling];
    }

    // update king position
    if (fromP == P_wk)      p.king[Co_White] = to;
    else if (fromP == P_bk) p.king[Co_Black] = to;

    // king capture : is that necessary ???
    if (toP == P_wk) p.king[Co_White] = INVALIDSQUARE;
    else if (toP == P_bk) p.king[Co_Black] = INVALIDSQUARE;

    STOP_AND_SUM_TIMER(MovePiece)
}

void applyNull(Searcher & , Position & pN) {
    START_TIMER
    pN.c = ~pN.c;
    pN.h ^= Zobrist::ZT[3][13];
    pN.h ^= Zobrist::ZT[4][13];
    if (pN.ep != INVALIDSQUARE) pN.h ^= Zobrist::ZT[pN.ep][13];
    pN.ep = INVALIDSQUARE;
    pN.lastMove = NULLMOVE;
    if ( pN.c == Co_White ) ++pN.moves;
    ++pN.halfmoves;

///@todo NNUE is that correct ???
#ifdef WITH_NNUE
    if (DynamicConfig::useNNUE){
      pN._accumulator->computed_score = false;
    }
#endif

    STOP_AND_SUM_TIMER(Apply)
}

bool applyMove(Position & p, const Move & m, bool noValidation){
    START_TIMER
    assert(VALIDMOVE(m));
#ifdef DEBUG_MATERIAL
    Position previous = p;
#endif
    const Square from   = Move2From(m); assert(squareOK(from));
    const Square to     = Move2To(m); assert(squareOK(to));
    const MType  type   = Move2Type(m); assert(moveTypeOK(type));
    const Piece  fromP  = p.board_const(from);
    const Piece  toP    = p.board_const(to);
    const int fromId    = fromP + PieceShift;
    const bool isCapNoEP= toP != P_none;
    Piece promPiece = P_none;
#ifdef DEBUG_APPLY
    if (!isPseudoLegal(p, m)) {
        Logging::LogIt(Logging::logError) << ToString(m) << " " << Move2Type(m);
        Logging::LogIt(Logging::logFatal) << "Apply error, not legal " << ToString(p);
        assert(false);
    }
#endif

#ifdef WITH_NNUE
    if (DynamicConfig::useNNUE){
       p._accumulator->computed_accumulation = false;
       p._accumulator->computed_score = false;
    }
    PieceId dp0 = PIECE_ID_NONE;
    PieceId dp1 = PIECE_ID_NONE;
    auto & dp = p._dirtyPiece;
    dp.dirty_num = 1; // at least one piece is changing ...
    Square rfrom = INVALIDSQUARE; // for castling
    Square rto   = INVALIDSQUARE; // for castling
    Square capSq = INVALIDSQUARE;
    if ( isCapture(type) ) capSq = to; // ep is fiexed later...
#endif

    switch(type){
    case T_std:
    case T_capture:
    case T_reserved:
        // update material
        if ( isCapNoEP ) {
            p.mat[~p.c][std::abs(toP)]--;
        }
        // update hash, BB, board and castling rights
        movePiece(p, from, to, fromP, toP, type == T_capture);
        break;
    case T_ep: {
        assert(p.ep != INVALIDSQUARE);
        assert(SQRANK(p.ep) == EPRank[p.c]);
        const Square epCapSq = p.ep + (p.c == Co_White ? -8 : +8);
#ifdef WITH_NNUE        
        capSq = epCapSq; // fix capture square in ep case
#endif
        assert(squareOK(epCapSq));
        BBTools::unSetBit(p, epCapSq, ~fromP); // BEFORE setting p.b new shape !!!
        _unSetBit(p.allPieces[fromP>0?Co_Black:Co_White],epCapSq);
        BBTools::unSetBit(p, from, fromP);
        _unSetBit(p.allPieces[fromP>0?Co_White:Co_Black],from);
        BBTools::setBit(p, to, fromP);
        _setBit(p.allPieces[fromP>0?Co_White:Co_Black],to);
        p.board(from) = P_none;
        p.board(to) = fromP;
        p.board(epCapSq) = P_none;

        p.h ^= Zobrist::ZT[from][fromId]; // remove fromP at from
        p.h ^= Zobrist::ZT[epCapSq][(p.c == Co_White ? P_bp : P_wp) + PieceShift]; // remove captured pawn
        p.h ^= Zobrist::ZT[to][fromId]; // add fromP at to

        p.ph ^= Zobrist::ZT[from][fromId]; // remove fromP at from
        p.ph ^= Zobrist::ZT[epCapSq][(p.c == Co_White ? P_bp : P_wp) + PieceShift]; // remove captured pawn
        p.ph ^= Zobrist::ZT[to][fromId]; // add fromP at to

        p.mat[~p.c][M_p]--;
    }
        break;
    case T_promq:
    case T_cappromq:
        MaterialHash::updateMaterialProm(p,to,type);
        promPiece = (p.c == Co_White ? P_wq : P_bq);
        movePiece(p, from, to, fromP, toP, type == T_cappromq,promPiece);
        break;
    case T_promr:
    case T_cappromr:
        MaterialHash::updateMaterialProm(p,to,type);
        promPiece = (p.c == Co_White ? P_wr : P_br);
        movePiece(p, from, to, fromP, toP, type == T_cappromr, promPiece);
        break;
    case T_promb:
    case T_cappromb:
        MaterialHash::updateMaterialProm(p,to,type);
        promPiece = (p.c == Co_White ? P_wb : P_bb);
        movePiece(p, from, to, fromP, toP, type == T_cappromb, promPiece);
        break;
    case T_promn:
    case T_cappromn:
        MaterialHash::updateMaterialProm(p,to,type);
        promPiece = (p.c == Co_White ? P_wn : P_bn);
        movePiece(p, from, to, fromP, toP, type == T_cappromn, promPiece);
        break;
    case T_wks:
        movePieceCastle<Co_White>(p,CT_OO,Sq_g1,Sq_f1);
#ifdef WITH_NNUE        
        rfrom = p.rooksInit[Co_White][CT_OO];
        rto = Sq_f1;
#endif
        break;
    case T_wqs:
        movePieceCastle<Co_White>(p,CT_OOO,Sq_c1,Sq_d1);
#ifdef WITH_NNUE        
        rfrom = p.rooksInit[Co_White][CT_OOO];
        rto = Sq_d1;
#endif
        break;
    case T_bks:
        movePieceCastle<Co_Black>(p,CT_OO,Sq_g8,Sq_f8);
#ifdef WITH_NNUE        
        rfrom = p.rooksInit[Co_Black][CT_OO];
        rto = Sq_f8;
#endif
        break;
    case T_bqs:
        movePieceCastle<Co_Black>(p,CT_OOO,Sq_c8,Sq_d8);
#ifdef WITH_NNUE        
        rfrom = p.rooksInit[Co_Black][CT_OOO];
        rto = Sq_d8;
#endif
        break;
    }

    if ( !noValidation && isAttacked(p,kingSquare(p)) ){
        STOP_AND_SUM_TIMER(Apply)
        return false; // this is the only legal move validation needed
    }

#ifdef WITH_NNUE
      if (DynamicConfig::useNNUE){
        if ( isCapture(type)){ // remove to piece (works also for ep)
            dp.dirty_num = 2; // 2 pieces changed
            dp1 = p.piece_id_on(capSq);
            dp.pieceId[1] = dp1;
            dp.old_piece[1] = p._evalList.piece_with_id(dp1);
            p._evalList.put_piece(dp1, capSq, PieceIdx(P_none));
            dp.new_piece[1] = p._evalList.piece_with_id(dp1);
        }

        if ( !isCastling(type)){ // move from piece
            dp0 = p.piece_id_on(from);
            dp.pieceId[0] = dp0;
            dp.old_piece[0] = p._evalList.piece_with_id(dp0);
            p._evalList.put_piece(dp0, to, PieceIdx(fromP));
            dp.new_piece[0] = p._evalList.piece_with_id(dp0);
        }

        if ( isPromotion(type)){ // change to piece type
            dp0 = p.piece_id_on(to);
            p._evalList.put_piece(dp0, to, PieceIdx(promPiece));
            dp.new_piece[0] = p._evalList.piece_with_id(dp0);
        }      

        if ( isCastling(type)){ 
            dp.dirty_num = 2; // 2 pieces moved
            dp0 = p.piece_id_on(from);
            dp1 = p.piece_id_on(rfrom);
            dp.pieceId[0] = dp0;
            dp.old_piece[0] = p._evalList.piece_with_id(dp0);
            p._evalList.put_piece(dp0, to, PieceIdx(p.c==Co_White?P_wk:P_bk));
            dp.new_piece[0] = p._evalList.piece_with_id(dp0);
            dp.pieceId[1] = dp1;
            dp.old_piece[1] = p._evalList.piece_with_id(dp1);
            p._evalList.put_piece(dp1, rto, PieceIdx(p.c==Co_White?P_wr:P_br));
            dp.new_piece[1] = p._evalList.piece_with_id(dp1);
        }
      }
#endif

    const bool pawnMove = abs(fromP) == P_wp;
    // update EP
    if (p.ep != INVALIDSQUARE) p.h  ^= Zobrist::ZT[p.ep][13];
    p.ep = INVALIDSQUARE;
    if (pawnMove && abs(to-from) == 16 ){
        p.ep = (from + to)/2;
        p.h  ^= Zobrist::ZT[p.ep][13];
    }
    assert(p.ep == INVALIDSQUARE || SQRANK(p.ep) == EPRank[~p.c]);

    // update color
    p.c = ~p.c;
    p.h ^= Zobrist::ZT[3][13] ; p.h  ^= Zobrist::ZT[4][13];

    // update game state
    if ( isCapNoEP || pawnMove) p.fifty = 0; // E.p covered by pawn move here ...
    else ++p.fifty;
    if ( p.c == Co_White ) ++p.moves;
    ++p.halfmoves;

    if ( isCaptureOrProm(type) ) MaterialHash::updateMaterialOther(p);
#ifdef DEBUG_MATERIAL
    Position::Material mat = p.mat;
    MaterialHash::initMaterial(p);
    if ( p.mat != mat ){ 
        Logging::LogIt(Logging::logWarn)  << "Material update error";
        Logging::LogIt(Logging::logWarn)  << "Material previous " << ToString(previous) << ToString(previous.mat);
        Logging::LogIt(Logging::logWarn)  << "Material computed " << ToString(p)        << ToString(p.mat);
        Logging::LogIt(Logging::logWarn)  << "Material incrementally updated " << ToString(mat);
        Logging::LogIt(Logging::logFatal) << "Last move " << ToString(p.lastMove) << " current move "  << ToString(m);
    }
#endif

#ifdef DEBUG_BITBOARD
    int count_bb1 = countBit(p.occupancy());
    int count_bb2 = 0;
    int count_board = 0;
    for (Square s = Sq_a1 ; s <= Sq_h8 ; ++s){
        if ( p.board_const(s) != P_none ) ++count_board;
    }
    for( Piece pp = P_bk ; pp <= P_wk ; ++pp){
       if ( pp == P_none) continue;
       BitBoard b = p.pieces_const(pp);
       const BitBoard bb = b;
       while(b){
           ++count_bb2;
           const Square s = popBit(b);
           if ( p.board_const(s) != pp ){
               Logging::LogIt(Logging::logWarn) << SquareNames[s];
               Logging::LogIt(Logging::logWarn) << ToString(p);
               Logging::LogIt(Logging::logWarn) << showBitBoard(bb);
               Logging::LogIt(Logging::logWarn) << (int) pp;
               Logging::LogIt(Logging::logWarn) << (int) p.board_const(s);
               Logging::LogIt(Logging::logWarn) << "last move " << ToString(p.lastMove);
               Logging::LogIt(Logging::logWarn) << " current move "  << ToString(m);
               Logging::LogIt(Logging::logFatal) << "Wrong bitboard ";
           }
       }
    }
    if ( count_bb1 != count_bb2 ){ 
        Logging::LogIt(Logging::logFatal) << "Wrong bitboard count (all/piece)" << count_bb1 << " " << count_bb2;
        Logging::LogIt(Logging::logWarn) << ToString(p);
        Logging::LogIt(Logging::logWarn) << showBitBoard(p.occupancy());
        for( Piece pp = P_bk ; pp <= P_wk ; ++pp){
            if ( pp == P_none) continue;
            Logging::LogIt(Logging::logWarn) << showBitBoard(p.pieces_const(pp));
        }
    }
    if ( count_board != count_bb1 ){ 
        Logging::LogIt(Logging::logFatal) << "Wrong bitboard count (board)" << count_board << " " << count_bb1;
        Logging::LogIt(Logging::logWarn) << ToString(p);
        Logging::LogIt(Logging::logWarn) << showBitBoard(p.occupancy());
    }
#endif
    p.lastMove = m;
    STOP_AND_SUM_TIMER(Apply)
    return true;
}

ScoreType randomMover(const Position & p, PVList & pv, bool isInCheck, Searcher & 
#ifdef WITH_GENFILE
                      context
#endif
) {
    MoveList moves;
    MoveGen::generate<MoveGen::GP_all>(p, moves, false);
    if (moves.empty()) return isInCheck ? -MATE : 0;
    static std::random_device rd;
    static std::mt19937 g(rd()); // here really random
    std::shuffle(moves.begin(), moves.end(),g);
    for (auto it = moves.begin(); it != moves.end(); ++it) {
        Position p2 = p;
        if (!applyMove(p2, *it)) continue;
        PVList childPV;
#ifdef WITH_GENFILE
        if ( DynamicConfig::genFen ) context.writeToGenFile(p2);
#endif        
        updatePV(pv, *it, childPV);
        const Square to = Move2To(*it);
        if (p.c == Co_White && to == p.king[Co_Black]) return MATE + 1;
        if (p.c == Co_Black && to == p.king[Co_White]) return MATE + 1;
        return 0; // found one valid move
    }
    // no move is valid ...
    return isInCheck ? -MATE : 0;
}

#ifdef DEBUG_PSEUDO_LEGAL
bool isPseudoLegal2(const Position & p, Move m); // forward decl
bool isPseudoLegal(const Position & p, Move m) {
    const bool b = isPseudoLegal2(p,m);
    if (b) {
        MoveList moves;
        MoveGen::generate<MoveGen::GP_all>(p, moves);
        bool found = false;
        for (auto it = moves.begin(); it != moves.end() && !found; ++it) if (sameMove(*it, m)) found = true;
        if (!found){
            std::cout << ToString(p) << "\n"  << ToString(m) << "\t" << m << std::endl;
            std::cout << SquareNames[Move2From(m)] << std::endl;
            std::cout << SquareNames[Move2To(m)] << std::endl;
            std::cout << (int)Move2Type(m) << std::endl;
            std::cout << Move2Score(m) << std::endl;
            std::cout << int(m & 0x0000FFFF) << std::endl;
            for (auto it = moves.begin(); it != moves.end(); ++it) std::cout << ToString(*it) <<"\t" << *it << "\t";
            std::cout << std::endl;
            std::cout << "Not a generated move !" << std::endl;
        }
    }
    return b;
}



bool isPseudoLegal2(const Position & p, Move m) { // validate TT move
#else
 bool isPseudoLegal(const Position & p, Move m) { // validate TT move
#endif
#ifdef DEBUG_PSEUDO_LEGAL
    #define PSEUDO_LEGAL_RETURN(b,r) { if (!b) std::cout << "isPseudoLegal2: " << r << std::endl; STOP_AND_SUM_TIMER(PseudoLegal) return b; }
#else
    #define PSEUDO_LEGAL_RETURN(b,r) { STOP_AND_SUM_TIMER(PseudoLegal) return b; }    
#endif
    START_TIMER
    if (!VALIDMOVE(m)) PSEUDO_LEGAL_RETURN(false,-1)
    const Square from = Move2From(m); assert(squareOK(from));
    const Piece fromP = p.board_const(from);
    if (fromP == P_none || (fromP > 0 && p.c == Co_Black) || (fromP < 0 && p.c == Co_White)) PSEUDO_LEGAL_RETURN(false,0)
    const Square to = Move2To(m); assert(squareOK(to));
    const Piece toP = p.board_const(to);
    if ((toP > 0 && p.c == Co_White) || (toP < 0 && p.c == Co_Black)) PSEUDO_LEGAL_RETURN(false,1)
    if ((Piece)std::abs(toP) == P_wk) PSEUDO_LEGAL_RETURN(false,2)
    const Piece fromPieceType = (Piece)std::abs(fromP);
    const MType t = Move2Type(m); assert(moveTypeOK(t));
    if ( t == T_reserved ) PSEUDO_LEGAL_RETURN(false,3)
    if (toP == P_none && (isCapture(t) && t!=T_ep)) PSEUDO_LEGAL_RETURN(false,4)
    if (toP != P_none && !isCapture(t)) PSEUDO_LEGAL_RETURN(false,5)
    if (t == T_ep && (p.ep == INVALIDSQUARE || fromPieceType != P_wp)) PSEUDO_LEGAL_RETURN(false,6)
    if (t == T_ep && p.board_const(p.ep + (p.c==Co_White?-8:+8)) != (p.c==Co_White?P_bp:P_wp)) PSEUDO_LEGAL_RETURN(false,7)
    if (isPromotion(m) && fromPieceType != P_wp) PSEUDO_LEGAL_RETURN(false,8)
    const BitBoard occupancy = p.occupancy();
    if (isCastling(m)) {
        if (p.c == Co_White) {
            if (t == T_wqs && (p.castling & C_wqs) && from == p.kingInit[Co_White] && fromP == P_wk && to == Sq_c1 && toP == P_none
                && (((BBTools::mask[p.king[Co_White]].between[Sq_c1] | BBSq_c1 | BBTools::mask[p.rooksInit[Co_White][CT_OOO]].between[Sq_d1] | BBSq_d1) 
                    & ~BBTools::mask[p.rooksInit[Co_White][CT_OOO]].bbsquare & ~BBTools::mask[p.king[Co_White]].bbsquare) & occupancy) == emptyBitBoard
                && !isAttacked(p, BBTools::mask[p.king[Co_White]].between[Sq_c1] | SquareToBitboard(p.king[Co_White]) | BBSq_c1))
                PSEUDO_LEGAL_RETURN(true,9)
            if (t == T_wks && (p.castling & C_wks) && from == p.kingInit[Co_White] && fromP == P_wk && to == Sq_g1 && toP == P_none
                && (((BBTools::mask[p.king[Co_White]].between[Sq_g1] | BBSq_g1 | BBTools::mask[p.rooksInit[Co_White][CT_OO]].between[Sq_f1] | BBSq_f1) 
                    & ~BBTools::mask[p.rooksInit[Co_White][CT_OO]].bbsquare & ~BBTools::mask[p.king[Co_White]].bbsquare) & occupancy) == emptyBitBoard
                && !isAttacked(p, BBTools::mask[p.king[Co_White]].between[Sq_g1] | SquareToBitboard(p.king[Co_White]) | BBSq_g1))
                PSEUDO_LEGAL_RETURN(true,10)
            PSEUDO_LEGAL_RETURN(false,11)
        }
        else {
            if (t == T_bqs && (p.castling & C_bqs) && from == p.kingInit[Co_Black] && fromP == P_bk && to == Sq_c8 && toP == P_none
                && (((BBTools::mask[p.king[Co_Black]].between[Sq_c8] | BBSq_c8 | BBTools::mask[p.rooksInit[Co_Black][CT_OOO]].between[Sq_d8] | BBSq_d8) 
                    & ~BBTools::mask[p.rooksInit[Co_Black][CT_OOO]].bbsquare & ~BBTools::mask[p.king[Co_Black]].bbsquare) & occupancy) == emptyBitBoard
                && !isAttacked(p, BBTools::mask[p.king[Co_Black]].between[Sq_c8] | SquareToBitboard(p.king[Co_Black]) | BBSq_c8))
                PSEUDO_LEGAL_RETURN(true,12)
            if (t == T_bks && (p.castling & C_bks) && from == p.kingInit[Co_Black] && fromP == P_bk && to == Sq_g8 && toP == P_none
                && (((BBTools::mask[p.king[Co_Black]].between[Sq_g8] | BBSq_g8 | BBTools::mask[p.rooksInit[Co_Black][CT_OO]].between[Sq_f8] | BBSq_f8) 
                    & ~BBTools::mask[p.rooksInit[Co_Black][CT_OO]].bbsquare & ~BBTools::mask[p.king[Co_Black]].bbsquare) & occupancy) == emptyBitBoard
                && !isAttacked(p, BBTools::mask[p.king[Co_Black]].between[Sq_g8] | SquareToBitboard(p.king[Co_Black]) | BBSq_g8))
                PSEUDO_LEGAL_RETURN(true,13)
            PSEUDO_LEGAL_RETURN(false,14)
        }
    }
    if (fromPieceType == P_wp) {
        if (t == T_ep && to != p.ep) PSEUDO_LEGAL_RETURN(false,15)
        if (t != T_ep && p.ep != INVALIDSQUARE && to == p.ep) PSEUDO_LEGAL_RETURN(false,16)
        if (!isPromotion(m) && SQRANK(to) == PromRank[p.c]) PSEUDO_LEGAL_RETURN(false,17)
        if (isPromotion(m) && SQRANK(to) != PromRank[p.c]) PSEUDO_LEGAL_RETURN(false,18)
        BitBoard validPush = BBTools::mask[from].push[p.c] & ~occupancy;
        if ((BBTools::mask[from].push[p.c] & occupancy) == emptyBitBoard) validPush |= BBTools::mask[from].dpush[p.c] & ~occupancy;
        if (validPush & SquareToBitboard(to)) PSEUDO_LEGAL_RETURN(true,19)
        const BitBoard validCap = BBTools::mask[from].pawnAttack[p.c] & ~p.allPieces[p.c];
        if ((validCap & SquareToBitboard(to)) && (( t != T_ep && toP != P_none) || (t == T_ep && to == p.ep && toP == P_none))) PSEUDO_LEGAL_RETURN(true,20)
        PSEUDO_LEGAL_RETURN(false,21)
    }
    if (fromPieceType != P_wk) {
        if ((BBTools::pfCoverage[fromPieceType - 1](from, occupancy, p.c) & SquareToBitboard(to)) != emptyBitBoard) PSEUDO_LEGAL_RETURN(true,22)
        PSEUDO_LEGAL_RETURN(false,23)
    }
    if ((BBTools::mask[p.king[p.c]].kingZone & SquareToBitboard(to)) != emptyBitBoard) PSEUDO_LEGAL_RETURN(true,24) // only king is not verified yet
    PSEUDO_LEGAL_RETURN(false,25)
}
