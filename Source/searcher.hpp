#pragma once

#include "definition.hpp"

#include "evalDef.hpp"
#include "material.hpp"
#include "score.hpp"
#include "smp.hpp"
#include "stats.hpp"
#include "tables.hpp"

/*!
 * Searcher struct store all the information needed by a search thread
 * Implements main search function (driver, pvs, qsearch, see, display to GUI, ...)
 * This was inspired from the former thread Stockfish style management
 *
 * Many things are templates here, so other hpp file are included at the bottom of this one.
 */
struct Searcher{
    bool stopFlag = true;
    MoveDifficultyUtil::MoveDifficulty moveDifficulty = MoveDifficultyUtil::MD_std;
    TimeType currentMoveMs = 777;
    
    TimeType getCurrentMoveMs(); // use this (and not the variable) to take emergency time into account !

    struct StackData{
       Position p;
       Hash h = nullHash;
       EvalData data = { 0, {0,0}, {0,0} };
       ScoreType eval = 0;
       MiniMove threat = INVALIDMINIMOVE;
    };
    std::array<StackData,MAX_PLY> stack;

    Stats stats;

    inline void DisplayStats()const{
       for(size_t k = 0 ; k < Stats::sid_maxid ; ++k){
           Logging::LogIt(Logging::logInfo) << Stats::Names[k] << " " << stats.counters[(Stats::StatId)k];
       }
    }

    std::vector<RootScores> rootScores;

    // used for move ordering
    Move previousBest = INVALIDMOVE;

    KillerT killerT;
    HistoryT historyT;
    CounterT counterT;
    DepthType nullMoveMinPly = 0;
    EvalScore contempt = 0;
    bool subSearch = false;

#ifdef WITH_GENFILE
    std::ofstream genStream;
    bool genFen = true;
    void writeToGenFile(const Position & p);
#endif

    void getCMHPtr(const unsigned int ply, CMHPtrArray & cmhPtr);
    [[nodiscard]] ScoreType getCMHScore(const Position & p, const Square from, const Square to, const CMHPtrArray & cmhPtr)const;

    [[nodiscard]] ScoreType drawScore();

    template <bool pvnode> ScoreType pvs(ScoreType alpha, 
                                         ScoreType beta, 
                                         const Position & p, 
                                         DepthType depth, 
                                         unsigned int ply, 
                                         PVList & pv, 
                                         DepthType & seldepth, 
                                         bool isInCheck, 
                                         bool cutNode, 
                                         bool canPrune, 
                                         const std::vector<MiniMove> * skipMoves = nullptr);

    [[nodiscard]] ScoreType qsearch(ScoreType alpha, 
                                    ScoreType beta, 
                                    const Position & p, 
                                    unsigned int ply, 
                                    DepthType & seldepth, 
                                    unsigned int qply, 
                                    bool qRoot, 
                                    bool pvnode, 
                                    signed char isInCheckHint = -1);

    // used for tuning not search !
    [[nodiscard]] ScoreType qsearchNoPruning(ScoreType alpha, 
                                             ScoreType beta, 
                                             const Position & p, 
                                             unsigned int ply, 
                                             DepthType & seldepth, 
                                             PVList * pv = nullptr);

    [[nodiscard]] bool SEE_GE(const Position & p, const Move & m, ScoreType threshold)const;

    [[nodiscard]] ScoreType SEE(const Position & p, const Move & m)const;

    PVList search(const Position & p, 
                  Move & m, 
                  DepthType & d, 
                  ScoreType & sc, 
                  DepthType & seldepth);

    template< bool withRep = true, bool isPv = true, bool INR = true> [[nodiscard]] MaterialHash::Terminaison interiorNodeRecognizer(const Position & p)const;

    [[nodiscard]] bool isRep(const Position & p, bool isPv)const;

    void displayGUI(DepthType depth, DepthType seldepth, ScoreType bestScore, const PVList & pv, int multipv, const std::string & mark = "");

    void idleLoop();

    void start();

    void wait();

    void search();

    [[nodiscard]] size_t id()const;
    [[nodiscard]] bool   isMainThread()const;

    Searcher(size_t n);

    ~Searcher();

    void setData(const ThreadData & d);
    [[nodiscard]] const ThreadData & getData()const;
    [[nodiscard]] ThreadData & getData();

    static std::atomic<bool> startLock;

    std::chrono::time_point<Clock> startTime;
  
    [[nodiscard]] bool searching()const;

    #pragma pack(push, 1)
    struct PawnEntry{
        BitBoard pawnTargets[2]   = {emptyBitBoard,emptyBitBoard};
        BitBoard holes[2]         = {emptyBitBoard,emptyBitBoard};
        BitBoard semiOpenFiles[2] = {emptyBitBoard,emptyBitBoard};
        BitBoard passed[2]        = {emptyBitBoard,emptyBitBoard};
        BitBoard openFiles        = emptyBitBoard;
        EvalScore score           = {0,0};
        ScoreType danger[2]       = {0,0};
        MiniHash h                = nullHash;
        inline void reset(){
            score = {0,0};
            danger[Co_White] = 0;   
            danger[Co_Black] = 0;
        }
    };
    #pragma pack(pop)

    static const unsigned long long int ttSizePawn;
    std::unique_ptr<PawnEntry[]> tablePawn = 0;

    void initPawnTable();

    void clearPawnTT();

    void clearGame();
    void clearSearch(bool forceCounterClear = false);

    [[nodiscard]] bool getPawnEntry(Hash h, PawnEntry *& pe);

    void prefetchPawn(Hash h);

private:
    ThreadData              _data;
    size_t                  _index;
    std::mutex              _mutex;
    std::condition_variable _cv;
    // next two MUST be initialized BEFORE _stdThread
    bool                    _exit;
    bool                    _searching;
    std::thread             _stdThread;
};

#include "searcherDraw.hpp"
#include "searcherPVS.hpp"
