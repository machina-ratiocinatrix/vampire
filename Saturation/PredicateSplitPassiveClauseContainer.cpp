/*
 * File PredicateSplitPassiveClauseContainer.cpp.
 *
 * This file is part of the source code of the software program
 * Vampire. It is protected by applicable
 * copyright laws.
 *
 * This source code is distributed under the licence found here
 * https://vprover.github.io/license.html
 * and in the source directory
 *
 * In summary, you are allowed to use Vampire for non-commercial
 * purposes but not allowed to distribute, modify, copy, create derivatives,
 * or use in competitions. 
 * For other uses of Vampire please contact developers for a different
 * licence, which we will make an effort to provide. 
 */

#include "PredicateSplitPassiveClauseContainer.hpp"

#include <numeric>
#include <string>
#include <algorithm>
#include <iterator>

#include "Shell/Options.hpp"
#include "Kernel/Clause.hpp"

namespace Saturation
{
using namespace Lib;
using namespace Kernel;

int computeGCD(int a, int b) {
    if (a == 0) {
      return b;
    }
    return computeGCD(b % a, a);
}
int computeLCM(int a, int b) {
  return (a*b)/computeGCD(a, b);
}

PredicateSplitPassiveClauseContainer::PredicateSplitPassiveClauseContainer(bool isOutermost, const Shell::Options& opt, vstring name) : PassiveClauseContainer(isOutermost, opt, name), _queues(), _ratios(), _cutoffs(), _balances(), _simulationBalances()
{
  CALL("PredicateSplitPassiveClauseContainer::PredicateSplitPassiveClauseContainer");

  // parse input-ratios
  vstringstream inputRatiosStream(_opt.splitQueueRatios());
  Lib::vvector<int> inputRatios;
  std::string currentRatio; 
  while (std::getline(inputRatiosStream, currentRatio, ','))
  {
    inputRatios.push_back(std::stoi(currentRatio));
  }

  // parse cutoffs
  vstringstream cutoffsStream(_opt.splitQueueCutoffs());
  std::string currentCutoff; 
  while (std::getline(cutoffsStream, currentCutoff, ','))
  {
    _cutoffs.push_back(std::stof(currentCutoff));
  }

  // sanity checks for ratios and cutoffs
  if (inputRatios.size() < 2) {
    USER_ERROR("Wrong usage of option '-sqr'. Needs to have at least two values (e.g. '10,1')");
  }
  if (inputRatios.size() != _cutoffs.size()) {
    USER_ERROR("The number of input ratios (supplied by option '-sqr') needs to match the number of cutoffs (supplied by option '-sqc')");
  }
  for (unsigned i = 0; i < inputRatios.size(); i++)
  {
    auto v = inputRatios[i];
    auto cutoff = _cutoffs[i];

    if(v <= 0) {
      USER_ERROR("Each ratio (supplied by option '-sqr') needs to be a positive integer");
    }
    if(! (0.0 <= cutoff && cutoff <= 1.0)) {
      USER_ERROR("Each cutoff value (supplied by option '-sqc') needs to be a float in the interval [0.0,1.0]");
    }
    if (i > 0 && cutoff <= _cutoffs[i-1])
    {
      USER_ERROR("The cutoff values (supplied by option '-sqc') must be strictly increasing");
    }
  }
  if (_cutoffs.back() != 1.0)
  {
      USER_ERROR("The last cutoff value (supplied by option '-sqc') must be 1.0");
  }

  // compute lcm, which will be used to compute reverse ratios
  auto lcm = 1;
  for (unsigned i = 0; i < inputRatios.size(); i++)
  {
    lcm = computeLCM(lcm, inputRatios[i]);
  }

  // initialize
  for (int i = 0; i < inputRatios.size(); i++)
  {
    _queues.push_back(Lib::make_unique<AWPassiveClauseContainer>(false, opt, "Queue " + Int::toString(_cutoffs[i])));
    _ratios.push_back(lcm / inputRatios[i]);
    _balances.push_back(0);
  }
}

PredicateSplitPassiveClauseContainer::~PredicateSplitPassiveClauseContainer() {
  CALL("PredicateSplitPassiveClauseContainer::~PredicateSplitPassiveClauseContainer");
}
  // heuristically compute likeliness that clause with inference inf occurs in proof
unsigned PredicateSplitPassiveClauseContainer::bestQueueHeuristics(Inference* inf) const {
  float th_ancestors = inf->th_ancestors;
  float all_ancestors = inf->all_ancestors;
  auto theoryRatio = th_ancestors / all_ancestors;
  auto niceness = theoryRatio;

  if (_opt.splitQueueFadeIn())
  {
    if (th_ancestors <= 2.0)
    {
      niceness = 0.0;
    }
    else if (th_ancestors == 3.0 && all_ancestors <= 6.0)
    {
      niceness = 0.5;
    }
    else if (th_ancestors == 4.0 && all_ancestors <= 5.0)
    {
      niceness = 0.8;
    }
  }
  
  // compute best queue clause should be placed in
  ASS(0.0 <= niceness && niceness <= 1.0);
  ASS(_cutoffs.back() == 1.0);
  for (unsigned i = 0; i < _cutoffs.size(); i++)
  {
    if (niceness <= _cutoffs[i])
    {
      return i;
    }
  }
}

void PredicateSplitPassiveClauseContainer::add(Clause* cl)
{
  CALL("PredicateSplitPassiveClauseContainer::add");
  ASS(cl->store() == Clause::PASSIVE);

  // add clause to all queues starting from best queue for clause
  auto bestQueueIndex = bestQueueHeuristics(cl->inference());
  for (unsigned i = bestQueueIndex; i < _queues.size(); i++)
  {
    _queues[i]->add(cl);
  }

  if (_isOutermost)
  {
    addedEvent.fire(cl);
  }

  ASS(cl->store() == Clause::PASSIVE);
}

void PredicateSplitPassiveClauseContainer::remove(Clause* cl)
{
  CALL("PredicateSplitPassiveClauseContainer::remove");
  if (_isOutermost)
  {
    ASS(cl->store()==Clause::PASSIVE);
  }
  // remove clause from all queues starting from best queue for clause
  auto bestQueueIndex = bestQueueHeuristics(cl->inference());
  for (unsigned i = bestQueueIndex; i < _queues.size(); i++)
  {
    _queues[i]->remove(cl);
  }

  if (_isOutermost)
  {
    ASS(cl->store()==Clause::PASSIVE);
    removedEvent.fire(cl);
    ASS(cl->store() != Clause::PASSIVE);
  }
}

bool PredicateSplitPassiveClauseContainer::isEmpty() const
{ 
  for (const auto& queue : _queues)
  {
    if (!queue->isEmpty())
    {
      return false;
    }
  }
  return true;
}

unsigned PredicateSplitPassiveClauseContainer::sizeEstimate() const
{ 
  ASS(!_queues.empty()); 
  // Note: If we use LRS, we lose the invariant that the last queue contains all clauses (since it can have stronger limits than the other queues).
  //       as a consequence the size of the last queue is only an estimate on the size.
  return _queues.back()->sizeEstimate();
}

Clause* PredicateSplitPassiveClauseContainer::popSelected()
{
  CALL("PredicateSplitPassiveClauseContainer::popSelected");
  // compute queue from which we will pick a clause:
  // choose queue using weighted round robin
  auto queueIndex = std::distance(_balances.begin(), std::min_element(_balances.begin(), _balances.end()));
  _balances[queueIndex] += _ratios[queueIndex];

  // if chosen queue is empty, use the next queue to the right
  // this succeeds in a non LRS-setting where we have the invariant that each clause from queue i is contained in queue j if i<j
  auto currIndex = queueIndex;
  while (currIndex < (long int)_queues.size() && _queues[currIndex]->isEmpty())
  {
    currIndex++;
  }
  // in the presence of LRS, we need to also consider the queues to the left as additional fallback (using the invar: at least one queue has at least one clause if popSelected is called)
  if (currIndex == (long int)_queues.size())
  {
    // fallback: try remaining queues, at least one of them must be nonempty
    ASS(queueIndex > 0); // otherwise we would already have searched through all queues
    currIndex = queueIndex - 1;
    while (_queues[currIndex]->isEmpty())
    {
      currIndex--;
      ASS(currIndex >= 0);
    }
  }
  ASS(!_queues[currIndex]->isEmpty());

  // pop clause from selected queue
  auto cl = _queues[currIndex]->popSelected();
  ASS(cl->store() == Clause::PASSIVE);

  // remove clause from all queues
  for (unsigned i = 0; i < _queues.size(); i++)
  {
    _queues[i]->remove(cl);
  }

  selectedEvent.fire(cl);

  return cl;
}

void PredicateSplitPassiveClauseContainer::simulationInit()
{
  CALL("PredicateSplitPassiveClauseContainer::simulationInit");

  _simulationBalances.clear();
  for (const auto& balance : _balances)
  {
    _simulationBalances.push_back(balance);
  }

  for (const auto& queue : _queues)
  {
    queue->simulationInit();
  }
}

bool PredicateSplitPassiveClauseContainer::simulationHasNext()
{
  CALL("PredicateSplitPassiveClauseContainer::simulationHasNext");
  bool hasNext = false;
  for (const auto& queue : _queues)
  {
    bool currHasNext = queue->simulationHasNext();
    hasNext = hasNext || currHasNext;
  }
  return hasNext;
}

void PredicateSplitPassiveClauseContainer::simulationPopSelected()
{
  CALL("PredicateSplitPassiveClauseContainer::simulationPopSelected");
  // compute queue from which we will pick a clause:
  // choose queue using weighted round robin
  auto queueIndex = std::distance(_simulationBalances.begin(), std::min_element(_simulationBalances.begin(), _simulationBalances.end()));
  _simulationBalances[queueIndex] += _ratios[queueIndex];

  // if chosen queue is empty, use the next queue to the right
  // this succeeds in a non LRS-setting where we have the invariant that each clause from queue i is contained in queue j if i<j
  auto currIndex = queueIndex;
  while (currIndex < (long int)_queues.size() && !_queues[currIndex]->simulationHasNext())
  {
    currIndex++;
  }
  // in the presence of LRS, we need to also consider the queues to the left as additional fallback (using the invar: at least one queue has at least one clause if popSelected is called)
  if (currIndex == (long int)_queues.size())
  {
    // fallback: try remaining queues, at least one of them must be nonempty
    ASS(queueIndex > 0); // otherwise we would already have searched through all queues
    currIndex = queueIndex - 1;
    while (!_queues[currIndex]->simulationHasNext())
    {
      currIndex--;
      ASS(currIndex >= 0);
    }
  }

  _queues[currIndex]->simulationPopSelected();
}

// returns whether at least one of the limits was tightened
bool PredicateSplitPassiveClauseContainer::setLimitsToMax()
{
  CALL("PredicateSplitPassiveClauseContainer::setLimitsToMax");
  bool tightened = false;
  for (const auto& queue : _queues)
  {
    bool currTightened = queue->setLimitsToMax();
    tightened = tightened || currTightened;
  }
  return tightened;
}

// returns whether at least one of the limits was tightened
bool PredicateSplitPassiveClauseContainer::setLimitsFromSimulation()
{
  CALL("PredicateSplitPassiveClauseContainer::setLimitsFromSimulation");
  bool tightened = false;
  for (const auto& queue : _queues)
  {
    bool currTightened = queue->setLimitsFromSimulation();
    tightened = tightened || currTightened;
  }
  return tightened;
}

void PredicateSplitPassiveClauseContainer::onLimitsUpdated()
{
  CALL("PredicateSplitPassiveClauseContainer::onLimitsUpdated");
  for (const auto& queue : _queues)
  {
    queue->onLimitsUpdated();
  }
}

bool PredicateSplitPassiveClauseContainer::ageLimited() const
{
  for (const auto& queue : _queues)
  {
    if (queue->ageLimited())
    {
      return true;
    }
  }
  return false;
}

bool PredicateSplitPassiveClauseContainer::weightLimited() const
{
  for (const auto& queue : _queues)
  {
    if (queue->weightLimited())
    {
      return true;
    }
  }
  return false;
}

// returns true if the cl fulfils at least one age-limit of a queue it is in
bool PredicateSplitPassiveClauseContainer::fulfilsAgeLimit(Clause* cl) const
{
  for (unsigned i = bestQueueHeuristics(cl->inference()); i < _queues.size(); i++)
  {
    auto& queue = _queues[i];
    if (queue->fulfilsAgeLimit(cl))
    {
      return true;
    }
  }
  return false;
}

// returns true if the cl fulfils at least one age-limit of a queue it is in
// note: w here denotes the weight as returned by weight().
// this method internally takes care of computing the corresponding weightForClauseSelection.
bool PredicateSplitPassiveClauseContainer::fulfilsAgeLimit(unsigned age, unsigned w, unsigned numeralWeight, bool derivedFromGoal, Inference* inference) const
{
  for (unsigned i = bestQueueHeuristics(inference); i < _queues.size(); i++)
  {
    auto& queue = _queues[i];
    if (queue->fulfilsAgeLimit(age, w, numeralWeight, derivedFromGoal, inference))
    {
      return true;
    }
  }
  return false;
}

// returns true if the cl fulfils at least one weight-limit of a queue it is in
bool PredicateSplitPassiveClauseContainer::fulfilsWeightLimit(Clause* cl) const
{
  for (unsigned i = bestQueueHeuristics(cl->inference()); i < _queues.size(); i++)
  {
    auto& queue = _queues[i];
    if (queue->fulfilsWeightLimit(cl))
    {
      return true;
    }
  }
  return false;
}
// returns true if the cl fulfils at least one weight-limit of a queue it is in
// note: w here denotes the weight as returned by weight().
// this method internally takes care of computing the corresponding weightForClauseSelection.
bool PredicateSplitPassiveClauseContainer::fulfilsWeightLimit(unsigned w, unsigned numeralWeight, bool derivedFromGoal, unsigned age, Inference* inference) const
{
  for (unsigned i = bestQueueHeuristics(inference); i < _queues.size(); i++)
  {
    auto& queue = _queues[i];
    if (queue->fulfilsWeightLimit(w, numeralWeight, derivedFromGoal, age, inference))
    {
      return true;
    }
  }
  return false;
}

bool PredicateSplitPassiveClauseContainer::childrenPotentiallyFulfilLimits(Clause* cl, unsigned upperBoundNumSelLits) const 
{
  // can't conlude any lower bounds on niceness of child-clause, so have to assume that it is potentially added to all queues.
  // In particular we need to check whether at least one of the queues could potentially select childrens of the clause.
  for (const auto& queue : _queues)
  {
    if (queue->childrenPotentiallyFulfilLimits(cl, upperBoundNumSelLits))
    {
      return true;
    }
  }
  return false;
}

};
