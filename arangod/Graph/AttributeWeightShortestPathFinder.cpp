////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "AttributeWeightShortestPathFinder.h"
#include "VocBase/Traverser.h"

#include <velocypack/Slice.h>

using namespace arangodb;
using namespace arangodb::graph;

AttributeWeightShortestPathFinder::SearcherTwoThreads::SearcherTwoThreads(
    AttributeWeightShortestPathFinder* pathFinder, ThreadInfo& myInfo,
    ThreadInfo& peerInfo, arangodb::velocypack::Slice const& start,
    ExpanderFunction expander, std::string const& id)
    : _pathFinder(pathFinder),
      _myInfo(myInfo),
      _peerInfo(peerInfo),
      _start(start),
      _expander(expander),
      _id(id) {}

void AttributeWeightShortestPathFinder::SearcherTwoThreads::insertNeighbor(
    Step* step, double newWeight) {
  MUTEX_LOCKER(locker, _myInfo._mutex);

  Step* s = _myInfo._pq.find(step->_vertex);

  // Not found, so insert it:
  if (s == nullptr) {
    step->setWeight(newWeight);
    _myInfo._pq.insert(step->_vertex, step);
    // step is consumed!
    return;
  }
  if (s->_done) {
    delete step;
    return;
  }
  if (s->weight() > newWeight) {
    s->_predecessor = step->_predecessor;
    s->_edge = step->_edge;
    _myInfo._pq.lowerWeight(s->_vertex, newWeight);
  }
  delete step;
}

void AttributeWeightShortestPathFinder::SearcherTwoThreads::lookupPeer(
    arangodb::velocypack::Slice& vertex, double weight) {
  MUTEX_LOCKER(locker, _peerInfo._mutex);

  Step* s = _peerInfo._pq.find(vertex);
  if (s == nullptr) {
    // Not found, nothing more to do
    return;
  }
  double total = s->weight() + weight;

  // Update the highscore:
  MUTEX_LOCKER(resultLocker, _pathFinder->_resultMutex);

  if (!_pathFinder->_highscoreSet || total < _pathFinder->_highscore) {
    _pathFinder->_highscoreSet = true;
    _pathFinder->_highscore = total;
    _pathFinder->_intermediate = vertex;
    _pathFinder->_intermediateSet = true;
  }

  // Now the highscore is set!

  // Did we find a solution together with the other thread?
  if (s->_done) {
    if (total <= _pathFinder->_highscore) {
      _pathFinder->_intermediate = vertex;
      _pathFinder->_intermediateSet = true;
    }
    // Hacki says: If the highscore was set, and even if
    // it is better than total, then this observation here
    // proves that it will never be better, so: BINGO.
    _pathFinder->_bingo = true;
    // We found a way, but somebody else found a better way, so
    // this is not the shortest path
    return;
  }

  // Did we find a solution on our own? This is for the
  // single thread case and for the case that the other
  // thread is too slow to even finish its own start vertex!
  if (s->weight() == 0) {
    // We have found the target, we have finished all
    // vertices with a smaller weight than this one (and did
    // not succeed), so this must be a best solution:
    _pathFinder->_intermediate = vertex;
    _pathFinder->_intermediateSet = true;
    _pathFinder->_bingo = true;
  }
}

void AttributeWeightShortestPathFinder::SearcherTwoThreads::run() {
  try {
    arangodb::velocypack::Slice v;
    Step* s;
    bool b;
    {
      MUTEX_LOCKER(locker, _myInfo._mutex);
      b = _myInfo._pq.popMinimal(v, s, true);
    }

    std::vector<Step*> neighbors;

    // Iterate while no bingo found and
    // there still is a vertex on the stack.
    while (!_pathFinder->_bingo && b) {
      neighbors.clear();
      _expander(v, neighbors);
      for (auto* neighbor : neighbors) {
        insertNeighbor(neighbor, s->weight() + neighbor->weight());
      }
      lookupPeer(v, s->weight());

      MUTEX_LOCKER(locker, _myInfo._mutex);
      Step* s2 = _myInfo._pq.find(v);
      s2->_done = true;
      b = _myInfo._pq.popMinimal(v, s, true);
    }
    // We can leave this loop only under 2 conditions:
    // 1) already bingo==true => bingo = true no effect
    // 2) This queue is empty => if there would be a
    //    path we would have found it here
    //    => No path possible. Set bingo, intermediate is empty.
    _pathFinder->_bingo = true;
  } catch (arangodb::basics::Exception const& ex) {
    _pathFinder->_resultCode = ex.code();
  } catch (std::bad_alloc const&) {
    _pathFinder->_resultCode = TRI_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    _pathFinder->_resultCode = TRI_ERROR_INTERNAL;
  }
}

void AttributeWeightShortestPathFinder::SearcherTwoThreads::start() {
  _thread = std::thread(&SearcherTwoThreads::run, this);
}

void AttributeWeightShortestPathFinder::SearcherTwoThreads::join() {
  _thread.join();
}

AttributeWeightShortestPathFinder::Searcher::Searcher(
    AttributeWeightShortestPathFinder* pathFinder, ThreadInfo& myInfo,
    ThreadInfo& peerInfo, arangodb::velocypack::Slice const& start,
    ExpanderFunction expander, std::string const& id)
    : _pathFinder(pathFinder),
      _myInfo(myInfo),
      _peerInfo(peerInfo),
      _start(start),
      _expander(expander),
      _id(id) {}

void AttributeWeightShortestPathFinder::Searcher::insertNeighbor(
    Step* step, double newWeight) {
  Step* s = _myInfo._pq.find(step->_vertex);

  // Not found, so insert it:
  if (s == nullptr) {
    step->setWeight(newWeight);
    _myInfo._pq.insert(step->_vertex, step);
    return;
  }
  if (!s->_done && s->weight() > newWeight) {
    s->_predecessor = step->_predecessor;
    s->_edge = step->_edge;
    _myInfo._pq.lowerWeight(s->_vertex, newWeight);
  }
  delete step;
}

void AttributeWeightShortestPathFinder::Searcher::lookupPeer(
    arangodb::velocypack::Slice& vertex, double weight) {
  Step* s = _peerInfo._pq.find(vertex);

  if (s == nullptr) {
    // Not found, nothing more to do
    return;
  }
  double total = s->weight() + weight;

  // Update the highscore:
  if (!_pathFinder->_highscoreSet || total < _pathFinder->_highscore) {
    _pathFinder->_highscoreSet = true;
    _pathFinder->_highscore = total;
    _pathFinder->_intermediate = vertex;
    _pathFinder->_intermediateSet = true;
  }

  // Now the highscore is set!

  // Did we find a solution together with the other thread?
  if (s->_done) {
    if (total <= _pathFinder->_highscore) {
      _pathFinder->_intermediate = vertex;
      _pathFinder->_intermediateSet = true;
    }
    // Hacki says: If the highscore was set, and even if
    // it is better than total, then this observation here
    // proves that it will never be better, so: BINGO.
    _pathFinder->_bingo = true;
    // We found a way, but somebody else found a better way,
    // so this is not the shortest path
    return;
  }

  // Did we find a solution on our own? This is for the
  // single thread case and for the case that the other
  // thread is too slow to even finish its own start vertex!
  if (s->weight() == 0) {
    // We have found the target, we have finished all
    // vertices with a smaller weight than this one (and did
    // not succeed), so this must be a best solution:
    _pathFinder->_intermediate = vertex;
    _pathFinder->_intermediateSet = true;
    _pathFinder->_bingo = true;
  }
}

bool AttributeWeightShortestPathFinder::Searcher::oneStep() {
  arangodb::velocypack::Slice v;
  Step* s = nullptr;
  bool b = _myInfo._pq.popMinimal(v, s, true);

  if (_pathFinder->_bingo || !b) {
    // We can leave this functino only under 2 conditions:
    // 1) already bingo==true => bingo = true no effect
    // 2) This queue is empty => if there would be a
    //    path we would have found it here
    //    => No path possible. Set bingo, intermediate is empty.
    _pathFinder->_bingo = true;
    return false;
  }

  TRI_ASSERT(s != nullptr);

  std::vector<Step*> neighbors;
  _expander(v, neighbors);
  for (Step* neighbor : neighbors) {
    insertNeighbor(neighbor, s->weight() + neighbor->weight());
  }
  lookupPeer(v, s->weight());

  Step* s2 = _myInfo._pq.find(v);
  s2->_done = true;
  return true;
}

AttributeWeightShortestPathFinder::AttributeWeightShortestPathFinder(
    ExpanderFunction&& forwardExpander, ExpanderFunction&& backwardExpander,
    bool bidirectional)
    : _highscoreSet(false),
      _highscore(0),
      _bingo(false),
      _resultCode(TRI_ERROR_NO_ERROR),
      _intermediateSet(false),
      _intermediate(),
      _forwardExpander(forwardExpander),
      _backwardExpander(backwardExpander),
      _bidirectional(bidirectional){};

bool AttributeWeightShortestPathFinder::shortestPath(
    arangodb::velocypack::Slice const& start,
    arangodb::velocypack::Slice const& target,
    arangodb::traverser::ShortestPath& result,
    std::function<void()> const& callback) {
  // For the result:
  result.clear();
  _highscoreSet = false;
  _highscore = 0;
  _bingo = false;
  _intermediateSet = false;

  // Forward with initialization:
  arangodb::velocypack::Slice emptyVertex;
  arangodb::velocypack::Slice emptyEdge;
  ThreadInfo forward;
  forward._pq.insert(start, new Step(start, emptyVertex, 0, emptyEdge));

  // backward with initialization:
  ThreadInfo backward;
  backward._pq.insert(target, new Step(target, emptyVertex, 0, emptyEdge));

  // Now the searcher threads:
  Searcher forwardSearcher(this, forward, backward, start, _forwardExpander,
                           "Forward");
  std::unique_ptr<Searcher> backwardSearcher;
  if (_bidirectional) {
    backwardSearcher.reset(new Searcher(this, backward, forward, target,
                                        _backwardExpander, "Backward"));
  }

  TRI_IF_FAILURE("TraversalOOMInitialize") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  int counter = 0;

  while (!_bingo) {
    if (!forwardSearcher.oneStep()) {
      break;
    }
    if (_bidirectional && !backwardSearcher->oneStep()) {
      break;
    }

    if (++counter == 10) {
      // check for abortion
      callback();
      counter = 0;
    }
  }

  if (!_bingo || _intermediateSet == false) {
    return false;
  }

  Step* s = forward._pq.find(_intermediate);
  result._vertices.emplace_back(_intermediate);

  // FORWARD Go path back from intermediate -> start.
  // Insert all vertices and edges at front of vector
  // Do NOT! insert the intermediate vertex
  while (!s->_predecessor.isNone()) {
    result._edges.push_front(s->_edge);
    result._vertices.push_front(s->_predecessor);
    s = forward._pq.find(s->_predecessor);
  }

  // BACKWARD Go path back from intermediate -> target.
  // Insert all vertices and edges at back of vector
  // Also insert the intermediate vertex
  s = backward._pq.find(_intermediate);
  while (!s->_predecessor.isNone()) {
    result._edges.emplace_back(s->_edge);
    result._vertices.emplace_back(s->_predecessor);
    s = backward._pq.find(s->_predecessor);
  }

  TRI_IF_FAILURE("TraversalOOMPath") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return true;
}

/* Here is a proof for the correctness of this algorithm:
 *
 * Assume we are looking for a shortest path from vertex A to vertex B.
 *
 * We do Dijkstra from both sides, thread 1 from A in forward direction and
 * thread 2 from B in backward direction. That is, we administrate a (hash)
 * table of distances from A to vertices in forward direction and one of
 * distances from B to vertices in backward direction.
 *
 * We get the following guarantees:
 *
 * When thread 1 is working on a vertex X, then it knows the distance w
 * from A to X.
 *
 * When thread 2 is working on a vertex Y, then it knows the distance v
 * from Y to B.
 *
 * When thread 1 is working on a vertex X at distance w from A, then it has
 * completed the work on all vertices X' at distance < w from A.
 *
 * When thread 2 is working on a vertex Y at distance v to B, then it has
 * completed the work on all vertices X' at (backward) distance < v to B.
 *
 * This all follows from the standard Dijkstra algorithm.
 *
 * Additionally, we do the following after we complete the normal work on a
 * vertex:
 *
 * Thread 1 checks for each vertex X at distance w from A whether thread 2
 * already knows it. If so, it makes sure that the highscore and intermediate
 * are set to the total length. Thread 2 does the analogous thing.
 *
 * If Thread 1 finds that vertex X (at distance v to B, say) has already
 * been completed by thread 2, then we call bingo. Thread 2 does the
 * analogous thing.
 *
 * We need to prove that the result is a shortest path.
 *
 * Assume that there is a shortest path of length <v+w from A to B. Let X'
 * be the latest vertex on this path with distance w' < w from A and let Y'
 * be the next one on the path. Then Y' is at distance w'+z' >= w from A
 * and thus at distance v' < v to B:
 *
 *    |     >=w      |   v'<v  |
 *    |  w'<w  |  z' |         |
 *    A -----> X' -> Y' -----> B
 *
 * Therefore, X' has already been completed by thread 1 and Y' has
 * already been completed by thread 2.
 *
 * Therefore, thread 1 has (in this temporal order) done:
 *
 *   1a: discover Y' and store it in table 1 under mutex 1
 *   1b: lookup X' in thread 2's table under mutex 2
 *   1c: mark X' as complete in table 1 under mutex 1
 *
 * And thread 2 has (in this temporal order) done:
 *
 *   2a: discover X' and store it in table 2 under mutex 2
 *   2b: lookup Y' in thread 1's table under mutex 1
 *   2c: mark Y' as complete in table 2 under mutex 2
 *
 * If 1b has happened before 2a, then 1a has happened before 2a and
 * thus 2b, so thread 2 has found the highscore w'+z'+v' < v+w.
 * Otherwise, 1b has happened after 2a and thus thread 1 has found the
 * highscore.
 *
 * Thus the highscore of this shortest path has already been set and the
 * algorithm is correct.
 */

bool AttributeWeightShortestPathFinder::shortestPathTwoThreads(
    arangodb::velocypack::Slice& start, arangodb::velocypack::Slice& target,
    arangodb::traverser::ShortestPath& result) {
  // For the result:
  result.clear();
  _highscoreSet = false;
  _highscore = 0;
  _bingo = false;

  // Forward with initialization:
  arangodb::velocypack::Slice emptyVertex;
  arangodb::velocypack::Slice emptyEdge;
  ThreadInfo forward;
  forward._pq.insert(start, new Step(start, emptyVertex, 0, emptyEdge));

  // backward with initialization:
  ThreadInfo backward;
  backward._pq.insert(target, new Step(target, emptyVertex, 0, emptyEdge));

  // Now the searcher threads:
  SearcherTwoThreads forwardSearcher(this, forward, backward, start,
                                     _forwardExpander, "Forward");
  std::unique_ptr<SearcherTwoThreads> backwardSearcher;
  if (_bidirectional) {
    backwardSearcher.reset(new SearcherTwoThreads(
        this, backward, forward, target, _backwardExpander, "Backward"));
  }

  TRI_IF_FAILURE("TraversalOOMInitialize") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  forwardSearcher.start();
  if (_bidirectional) {
    backwardSearcher->start();
  }
  forwardSearcher.join();
  if (_bidirectional) {
    backwardSearcher->join();
  }

  // check error code returned by the threads
  int res = _resultCode.load();

  if (res != TRI_ERROR_NO_ERROR) {
    // one of the threads caught an exception
    THROW_ARANGO_EXCEPTION(res);
  }

  if (!_bingo || _intermediateSet == false) {
    return false;
  }

  Step* s = forward._pq.find(_intermediate);
  result._vertices.emplace_back(_intermediate);

  // FORWARD Go path back from intermediate -> start.
  // Insert all vertices and edges at front of vector
  // Do NOT! insert the intermediate vertex
  while (!s->_predecessor.isNone()) {
    result._edges.push_front(s->_edge);
    result._vertices.push_front(s->_predecessor);
    s = forward._pq.find(s->_predecessor);
  }

  // BACKWARD Go path back from intermediate -> target.
  // Insert all vertices and edges at back of vector
  // Also insert the intermediate vertex
  s = backward._pq.find(_intermediate);
  while (!s->_predecessor.isNone()) {
    result._edges.emplace_back(s->_edge);
    result._vertices.emplace_back(s->_predecessor);
    s = backward._pq.find(s->_predecessor);
  }

  TRI_IF_FAILURE("TraversalOOMPath") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return true;
}