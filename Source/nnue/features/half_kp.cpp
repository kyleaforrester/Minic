/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2020 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

//Definition of input features HalfKP of NNUE evaluation function

#include "definition.hpp"

#ifdef WITH_NNUE

#include "half_kp.h"
#include "index_list.h"

#include "position.hpp"
#include "bitboard.hpp"

namespace NNUE::Features {

  // Orient a square according to perspective (flip rank for black)
  inline Square orient(Color perspective, Square s) {
    return Square(int(s) ^ (bool(perspective) * Sq_h8));
  }

  // Find the index of the feature quantity from the king position and PieceSquare
  template <Side AssociatedKing>
  inline IndexType HalfKP<AssociatedKing>::MakeIndex(
      Color perspective, Square s, Piece pc, Square ksq) {

    return IndexType(orient(perspective, s) + kpp_board_index[PieceIdx(pc)][perspective] + PS_END * ksq);
  }

  // Get a list of indices for active features
  template <Side AssociatedKing>
  void HalfKP<AssociatedKing>::AppendActiveIndices(
      const Position& pos, Color perspective, IndexList* active) {

    Square ksq = orient(perspective, pos.king[AssociatedKing == Side::kFriend ? perspective : ~perspective]);
    BitBoard bb = pos.occupancy() & ~(pos.whiteKing()|pos.blackKing());
    while (bb) {
      const Square s = popBit(bb);
      active->push_back(MakeIndex(perspective, s, pos.board_const(s), ksq));
    }
  }

  // Get a list of indices for recently changed features
  template <Side AssociatedKing>
  void HalfKP<AssociatedKing>::AppendChangedIndices(
      const Position& pos, Color perspective,
      IndexList* removed, IndexList* added) {

    Square ksq = orient(perspective, pos.king[AssociatedKing == Side::kFriend ? perspective : ~perspective]);
    const auto& dp = pos.dirtyPiece();
    for (int i = 0; i < dp.dirty_num; ++i) {
      const Piece pc = dp.piece[i];
      if (std::abs(pc) == P_wk) continue;
      if (dp.from[i] != SQ_NONE)
        removed->push_back(MakeIndex(perspective, dp.from[i], pc, ksq));
      if (dp.to[i] != SQ_NONE)
        added->push_back(MakeIndex(perspective, dp.to[i], pc, ksq));
    }
  }

  template class HalfKP<Side::kFriend>;
  template class HalfKP<Side::kEnemy>;

}  // namespace NNUE::Features

#endif
