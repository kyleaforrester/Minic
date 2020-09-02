#include "position.hpp"

#include "bitboardTools.hpp"
#include "dynamicConfig.hpp"
#include "hash.hpp"
#include "logging.hpp"
#include "material.hpp"
#include "moveGen.hpp"

#ifdef WITH_NNUE
#include <cstddef>
#include "nnue/nnue_accumulator.h"
#endif

template < typename T > T readFromString(const std::string & s){ std::stringstream ss(s); T tmp; ss >> tmp; return tmp;}

bool readFEN(const std::string & fen, Position & p, bool silent, bool withMoveCount){
    static Position defaultPos;
    p = defaultPos;
#ifdef WITH_NNUE
    if ( DynamicConfig::useNNUE) p.resetAccumulator();
#endif
    std::vector<std::string> strList;
    std::stringstream iss(fen);
    std::copy(std::istream_iterator<std::string>(iss), std::istream_iterator<std::string>(), back_inserter(strList));

    if ( !silent) Logging::LogIt(Logging::logInfo) << "Reading fen " << fen ;

    // reset position
    p.h = nullHash; p.ph = nullHash;
    for(Square k = 0 ; k < NbSquare ; ++k) p.board(k) = P_none;

    Square j = 1, i = 0;
#ifdef WITH_NNUE
    PieceId piece_id, next_piece_id = PIECE_ID_ZERO;
#endif
    while ((j <= NbSquare) && (i <= (char)strList[0].length())){
        char letter = strList[0].at(i);
        ++i;
        const Square k = (7 - (j - 1) / 8) * 8 + ((j - 1) % 8);
        bool pieceAdded = false;
        switch (letter) {
        case 'p': pieceAdded = true; p.board(k) = P_bp; break;
        case 'r': pieceAdded = true; p.board(k) = P_br; break;
        case 'n': pieceAdded = true; p.board(k) = P_bn; break;
        case 'b': pieceAdded = true; p.board(k) = P_bb; break;
        case 'q': pieceAdded = true; p.board(k) = P_bq; break;
        case 'k': pieceAdded = true; p.board(k) = P_bk; p.king[Co_Black] = k; break;
        case 'P': pieceAdded = true; p.board(k) = P_wp; break;
        case 'R': pieceAdded = true; p.board(k) = P_wr; break;
        case 'N': pieceAdded = true; p.board(k) = P_wn; break;
        case 'B': pieceAdded = true; p.board(k) = P_wb; break;
        case 'Q': pieceAdded = true; p.board(k) = P_wq; break;
        case 'K': pieceAdded = true; p.board(k) = P_wk; p.king[Co_White] = k; break;
        case '/': j--; break;
        case '1': break;
        case '2': j++; break;
        case '3': j += 2; break;
        case '4': j += 3; break;
        case '5': j += 4; break;
        case '6': j += 5; break;
        case '7': j += 6; break;
        case '8': j += 7; break;
        default: Logging::LogIt(Logging::logFatal) << "FEN ERROR -1 : invalid character in fen string :" << letter << "\n" << fen;
        }
#ifdef WITH_NNUE
        if ( /*DynamicConfig::useNNUE &&*/ pieceAdded){ ///@todo can be done even if !DynamicConfig::useNNUE to avoid issue ?
              piece_id =
                (p.board(k) == P_wk) ? PIECE_ID_WKING :
                (p.board(k) == P_bk) ? PIECE_ID_BKING :
                next_piece_id++;
              p._evalList.put_piece(piece_id, k, PieceIdx(p.board(k)));
        }
#endif
        j++;
    }

    if ( p.king[Co_White] == INVALIDSQUARE || p.king[Co_Black] == INVALIDSQUARE ) { Logging::LogIt(Logging::logFatal) << "FEN ERROR 0 : missing king" ; return false; }

    p.c = Co_White; // set the turn; default is white
    if (strList.size() >= 2){
        if (strList[1] == "w")      p.c = Co_White;
        else if (strList[1] == "b") p.c = Co_Black;
        else { Logging::LogIt(Logging::logFatal) << "FEN ERROR 1 : bad color" ; return false; }
    }

    // Initialize all castle possibilities (default is none)
    p.castling = C_none;
    if (strList.size() >= 3){
        bool found = false;
        //Logging::LogIt(Logging::logInfo) << "is not FRC";
        if (strList[2].find('K') != std::string::npos){ p.castling |= C_wks; found = true; }
        if (strList[2].find('Q') != std::string::npos){ p.castling |= C_wqs; found = true; }
        if (strList[2].find('k') != std::string::npos){ p.castling |= C_bks; found = true; }
        if (strList[2].find('q') != std::string::npos){ p.castling |= C_bqs; found = true; }

        if (!found){
           for ( const char & cr : strList[2] ){
               if ( (cr >= 'A' && cr <= 'H') || (cr >= 'a' && cr <= 'h') ){
                  Logging::LogIt(Logging::logInfo) << "Found FRC like castling " << cr;
                  const Color c = std::isupper(cr) ? Co_White : Co_Black;
                  const char kf = std::toupper(FileNames[SQFILE(p.king[c])].at(0));
                  if ( std::toupper(cr) > kf ) { p.castling |= (c==Co_White ? C_wks:C_bks); }
                  else                         { p.castling |= (c==Co_White ? C_wqs:C_bqs); }
                  DynamicConfig::FRC = true; // force FRC !
                  found = true;
               }
           }
           if ( found ) Logging::LogIt(Logging::logInfo) << "FRC position found, activating FRC";
        }
        if (strList[2].find('-') != std::string::npos){ found = true; /*Logging::LogIt(Logging::logInfo) << "No castling right given" ;*/}
        if ( ! found ){ 
            if ( !silent) Logging::LogIt(Logging::logWarn) << "No castling right given" ; 
        }
        else{ ///@todo detect illegal stuff in here
            p.kingInit[Co_White] = p.king[Co_White];
            p.kingInit[Co_Black] = p.king[Co_Black];
            if ( p.castling & C_wqs ) { for( Square s = Sq_a1 ; s <= Sq_h1 ; ++s ){ if ( s < p.king[Co_White] && p.board_const(s)==P_wr ) { p.rooksInit[Co_White][CT_OOO] = s; break; } } }
            if ( p.castling & C_wks ) { for( Square s = Sq_a1 ; s <= Sq_h1 ; ++s ){ if ( s > p.king[Co_White] && p.board_const(s)==P_wr ) { p.rooksInit[Co_White][CT_OO]  = s; break; } } }
            if ( p.castling & C_bqs ) { for( Square s = Sq_a8 ; s <= Sq_h8 ; ++s ){ if ( s < p.king[Co_Black] && p.board_const(s)==P_br ) { p.rooksInit[Co_Black][CT_OOO] = s; break; } } }
            if ( p.castling & C_bks ) { for( Square s = Sq_a8 ; s <= Sq_h8 ; ++s ){ if ( s > p.king[Co_Black] && p.board_const(s)==P_br ) { p.rooksInit[Co_Black][CT_OO]  = s; break; } } }
        }
    }
    else if ( !silent) Logging::LogIt(Logging::logInfo) << "No castling right given" ;

    // read en passant and save it (default is invalid)
    p.ep = INVALIDSQUARE;
    if ((strList.size() >= 4) && strList[3] != "-" ){
        if (strList[3].length() >= 2){
            if ((strList[3].at(0) >= 'a') && (strList[3].at(0) <= 'h') && ((strList[3].at(1) == '3') || (strList[3].at(1) == '6'))) p.ep = stringToSquare(strList[3]);
            else { Logging::LogIt(Logging::logFatal) << "FEN ERROR 2 : bad en passant square : " << strList[3] ; return false; }
        }
        else{ Logging::LogIt(Logging::logFatal) << "FEN ERROR 3 : bad en passant square : " << strList[3] ; return false; }
    }
    else if ( !silent) Logging::LogIt(Logging::logInfo) << "No en passant square given" ;

    assert(p.ep == INVALIDSQUARE || (SQRANK(p.ep) == 2 || SQRANK(p.ep) == 5));

    // read 50 moves rules
    if (withMoveCount && strList.size() >= 5) p.fifty = (unsigned char)readFromString<int>(strList[4]);
    else p.fifty = 0;

    // read number of move
    if (withMoveCount && strList.size() >= 6) p.moves = (unsigned char)readFromString<int>(strList[5]);
    else p.moves = 1;

    if (p.moves < 1) { // fix a LittleBlitzer bug here ...
        Logging::LogIt(Logging::logInfo) << "Wrong move counter " << (int)p.moves << " using 1 instead";
        p.moves = 1;
    }

    p.halfmoves = (int(p.moves) - 1) * 2 + 1 + (p.c == Co_Black ? 1 : 0);

    initCaslingPermHashTable(p);

    BBTools::setBitBoards(p);
    MaterialHash::initMaterial(p);
    p.h = computeHash(p);
    p.ph = computePHash(p);

    return true;
}

#ifdef WITH_NNUE

const DirtyPiece & Position::dirtyPiece() const{
    return _dirtyPiece;
}

const EvalList* Position::eval_list() const { 
    return &_evalList; 
}

NNUE::Accumulator & Position::accumulator()const{
    assert(_accumulator);
    return *_accumulator;
}

NNUE::Accumulator * Position::previousAccumulatorPtr()const{
    return _previousAccumulator;
}

PieceId Position::piece_id_on(Square sq) const{
  //assert(board_const(sq) != P_none); // piece is moved before NNUE things in apply !!!
  PieceId pid = _evalList.piece_id_list[sq];
  assert(PieceIdOK(pid));
  return pid;
}

Position & Position::operator =(const Position & p){

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    std::memcpy(this, &p, offsetof(Position, _accumulator));
#pragma GCC diagnostic pop
    if (DynamicConfig::useNNUE){
       if ( _accumulator ) delete _accumulator;
       _accumulator = new NNUE::Accumulator();
       _previousAccumulator = p._accumulator;
    }
    return *this;
}

Position::Position(const Position & p){
    //assert(false);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    std::memcpy(this, &p, offsetof(Position, _accumulator));
#pragma GCC diagnostic pop
    if (DynamicConfig::useNNUE){
       _accumulator = new NNUE::Accumulator();
       _previousAccumulator = p._accumulator;    
    }
}

void Position::resetAccumulator(){
    if (DynamicConfig::useNNUE){
       if ( _accumulator) delete _accumulator;
       _accumulator = new NNUE::Accumulator();
       _previousAccumulator = nullptr;    
    }
}

#endif

Position::~Position(){
#ifdef WITH_NNUE
    if (DynamicConfig::useNNUE){
       if (_accumulator) delete _accumulator;
    }
#endif
}

Position::Position(){
#ifdef WITH_NNUE
    resetAccumulator();
#endif
}

Position::Position(const std::string & fen, bool withMoveCount){
#ifdef WITH_NNUE
    resetAccumulator();
#endif
    readFEN(fen,*this,true,withMoveCount);
}