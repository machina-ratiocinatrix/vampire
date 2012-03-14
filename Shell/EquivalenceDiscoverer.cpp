/**
 * @file EquivalenceDiscoverer.cpp
 * Implements class EquivalenceDiscoverer.
 */

#include "Lib/DArray.hpp"
#include "Lib/Environment.hpp"

#include "Kernel/Clause.hpp"
#include "Kernel/Formula.hpp"
#include "Kernel/FormulaUnit.hpp"
#include "Kernel/Grounder.hpp"
#include "Kernel/SortHelper.hpp"
#include "Kernel/Term.hpp"

#include "SAT/ISSatSweeping.hpp"
#include "SAT/Preprocess.hpp"
#include "SAT/SATInference.hpp"
#include "SAT/TWLSolver.hpp"

#include "EquivalenceDiscoverer.hpp"
#include "PDInliner.hpp"
#include "PDUtils.hpp"
#include "Preprocess.hpp"

namespace Shell
{

using namespace Kernel;
using namespace SAT;

EquivalenceDiscoverer::EquivalenceDiscoverer(bool normalizeForSAT, unsigned satConflictCountLimit,
    bool checkOnlyDefinitionHeads)
    : _satConflictCountLimit(satConflictCountLimit),
      _checkOnlyDefinitionHeads(checkOnlyDefinitionHeads),
      _restrictedRange(false),
      _gnd(normalizeForSAT),
      _maxSatVar(0)
{
  CALL("EquivalenceDiscoverer::EquivalenceDiscoverer");

  _useISS = true;
  _solver = new TWLSolver(*env.options, false);
}

/**
 * Restrict equivalence discovery only to equivalences between elements of @c set1 and @c set2.
 */
void EquivalenceDiscoverer::setRestrictedRange(LiteralIterator set1, LiteralIterator set2)
{
  CALL("EquivalenceDiscoverer::setRestrictedRange");

  _restrictedRange = true;
  _restrictedRangeSet1.loadFromIterator(getMappingIteratorKnownRes<Literal*>(set1, Literal::positiveLiteral));
  _restrictedRangeSet2.loadFromIterator(getMappingIteratorKnownRes<Literal*>(set2, Literal::positiveLiteral));
}

void EquivalenceDiscoverer::addGrounding(Clause* cl)
{
  CALL("EquivalenceDiscoverer::addGrounding");

  unsigned clen = cl->length();
  static DArray<Literal*> normLits;
  normLits.ensure(clen);

  SATClause* scl = _gnd.groundNonProp(cl, normLits.array());
  scl->setInference(new FOConversionInference(cl));
  _satClauses.push(scl);

  for(unsigned i=0; i<clen; ++i) {
    Literal* nlit = normLits[i];
    SATLiteral slit = (*scl)[i];

    if(slit.var()>_maxSatVar) {
      _maxSatVar = slit.var();
    }

    _s2f.insert(slit, nlit);
  }
}

/**
 * Return true if literal @c l is eligible for equivalence discovery
 *
 * (we will attempt to discover equivalences between pairs of eligible literals)
 */
bool EquivalenceDiscoverer::isEligible(Literal* l)
{
  CALL("EquivalenceDiscoverer::isEligible");

  if(_restrictedRange) {
    return _restrictedRangeSet1.contains(l) || _restrictedRangeSet2.contains(l);
  }
  else {
    if(env.signature->getPredicate(l->functor())->introduced()) {
      return false;
    }

    return !_checkOnlyDefinitionHeads || PDUtils::isDefinitionHead(l);
  }
}

void EquivalenceDiscoverer::collectRelevantLits()
{
  CALL("EquivalenceDiscoverer::collectRelevantLits");

  DHSet<SATLiteral> seen;

  SATClauseStack::ConstIterator scit(_filteredSatClauses);
  while(scit.hasNext()) {
    SATClause* sc = scit.next();
    SATClause::Iterator slitIt(*sc);
    while(slitIt.hasNext()) {
      SATLiteral slit = slitIt.next();

      SATLiteral spLit = slit.positive();
      if(!seen.insert(spLit)) { continue; }

      //positive polarity of the SAT literal should be in the s2f map because we have
      //removed all pure literals before calling this function in the getEquivalences()
      Literal* npLit = _s2f.get(spLit);

      if(!isEligible(npLit)) { continue; }

      _eligibleSatLits.push(spLit);
    }
  }
}

void EquivalenceDiscoverer::loadInitialAssignment()
{
  CALL("EquivalenceDiscoverer::loadInitialAssignment");

  _initialAssignment.ensure(_maxSatVar+1);
  for(unsigned i=1; i<=_maxSatVar; i++) {
    SATSolver::VarAssignment asgn = _solver->getAssignment(i);
    switch(asgn) {
    case SATSolver::DONT_CARE:
      break;
    case SATSolver::FALSE:
    case SATSolver::TRUE:
      _initialAssignment.insert(i, asgn==SATSolver::TRUE);
      break;
    case SATSolver::NOT_KNOWN:
    default:
      ASSERTION_VIOLATION;
    }
  }
}

UnitList* EquivalenceDiscoverer::getEquivalences(ClauseIterator clauses)
{
  CALL("EquivalenceDiscoverer::getEquivalences");

  DArray<Literal*> norm;
  while(clauses.hasNext()) {
    Clause* cl = clauses.next();
    addGrounding(cl);
  }

  LOG("pp_ed_progress","groundings added");

  _filteredSatClauses.loadFromIterator(
      SAT::Preprocess::filterPureLiterals(_maxSatVar+1,
	  SAT::Preprocess::removeDuplicateLiterals(pvi(SATClauseStack::Iterator(_satClauses)))));

  collectRelevantLits();

  LOG("pp_ed_progress","relevant literals collected");

  _solver->ensureVarCnt(_maxSatVar+1);
  _solver->addClauses(pvi(SATClauseStack::Iterator(_filteredSatClauses)));

  LOG("pp_ed_progress","grounded clauses added to SAT solver");

  if(_solver->getStatus()==SATSolver::UNSATISFIABLE) {
    //we might have built a refutation clause here but this is highly unlikely case anyway...
    return 0;
  }
  ASS_EQ(_solver->getStatus(),SATSolver::SATISFIABLE);

  loadInitialAssignment();

  //the actual equivalence finding

  LOG("pp_ed_progress","starting equivalence discovery among "<<_eligibleSatLits.size()<<" atoms");

  UnitList* res = 0;
  if(_useISS) {
    discoverISSatEquivalences(res);
  }
  else {
    unsigned elCnt = _eligibleSatLits.size();
    LOG("pp_ed_progress","literals to process: "<<elCnt);
    for(unsigned i=0; i<elCnt; ++i) {
      SATLiteral l1 = _eligibleSatLits[i];
      if(_restrictedRange && !_restrictedRangeSet1.contains(_s2f.get(l1.positive()))) {
        continue;
      }
      LOG("pp_ed_progress","processing literal "<<(*getFOLit(l1)));
      for(unsigned j=i+1; j<elCnt; ++j) {
        SATLiteral l2 = _eligibleSatLits[j];
        ASS_NEQ(l1,l2);
        if(_restrictedRange && !_restrictedRangeSet2.contains(_s2f.get(l2.positive()))) {
          continue;
        }
        if(areEquivalent(l1,l2) && handleEquivalence(l1, l2, res)) {
  	break;
        }
        if(areEquivalent(l1,l2.opposite()) && handleEquivalence(l1, l2.opposite(), res)) {
  	break;
        }
      }
    }
  }
  LOG("pp_ed_progress","finished");

  return res;
}

SATSolver& EquivalenceDiscoverer::getProofRecordingSolver()
{
  CALL("EquivalenceDiscoverer::getProofRecordingSolver");

  if(!_proofRecordingSolver) {

    //we need to make copies of clauses as the same clause object
    //cannot be in two SAT solver objects at once (SAT solvers can modify clauses)

    SATClauseStack clauseCopies;
    SATClauseStack::Iterator cit(_filteredSatClauses);
    while(cit.hasNext()) {
      SATClause* cl = cit.next();
      SATClause* clCopy = SATClause::copy(cl);
      clauseCopies.push(clCopy);
    }

    _proofRecordingSolver = new TWLSolver(*env.options, true);
    _proofRecordingSolver->ensureVarCnt(_maxSatVar+1);
    _proofRecordingSolver->addClauses(pvi(SATClauseStack::Iterator(clauseCopies)), true);
  }
  ASS_NEQ(_proofRecordingSolver->getStatus(), SATSolver::UNSATISFIABLE);
  ASS(!_proofRecordingSolver->hasAssumptions());
  return *_proofRecordingSolver;
}

void EquivalenceDiscoverer::getImplicationPremises(SATLiteral l1, SATLiteral l2, Stack<UnitSpec>& acc)
{
  CALL("EquivalenceDiscoverer::getImplicationPremises");

  SATSolver& ps = getProofRecordingSolver();
  ASS(!ps.hasAssumptions());

  ps.addAssumption(l1,true);
  ps.addAssumption(l2.opposite(),false);
  ASS_EQ(ps.getStatus(), SATSolver::UNSATISFIABLE);
  SATClause* ref = ps.getRefutation();
  SATInference::collectFOPremises(ref, acc);
  ps.retractAllAssumptions();
}

Inference* EquivalenceDiscoverer::getEquivInference(SATLiteral l1, SATLiteral l2)
{
  CALL("EquivalenceDiscoverer::getEquivInference");

  static Stack<UnitSpec> premises;
  ASS(premises.isEmpty());

  getImplicationPremises(l1, l2, premises);
  getImplicationPremises(l2, l1, premises);

  UnitList* premLst = 0;

  while(premises.isNonEmpty()) {
    UnitSpec us = premises.pop();
    ASS(us.withoutProp());
    UnitList::push(us.unit(), premLst);
  }
  return new InferenceMany(Inference::EQUIVALENCE_DISCOVERY, premLst);
}

void EquivalenceDiscoverer::discoverISSatEquivalences(UnitList*& eqAcc)
{
  CALL("EquivalenceDiscoverer::discoverISSatEquivalences");
  ASS_EQ(_solver->getStatus(),SATSolver::SATISFIABLE);

  ISSatSweeping sswp(_maxSatVar+1, *_solver,
      pvi( getMappingIteratorKnownRes<int>(SATLiteralStack::ConstIterator(_eligibleSatLits), satLiteralVar) ));

  Stack<ISSatSweeping::Equiv>::ConstIterator eqIt(sswp.getEquivalences());
  while(eqIt.hasNext()) {
    ISSatSweeping::Equiv eq = eqIt.next();
    handleEquivalence(eq.first, eq.second, eqAcc);
  }
}

Literal* EquivalenceDiscoverer::getFOLit(SATLiteral slit) const
{
  CALL("EquivalenceDiscoverer::getFOLit");

  Literal* res;
  if(_s2f.find(slit, res)) {
    return res;
  }
  res = Literal::complementaryLiteral(_s2f.get(slit.opposite()));
  return res;
}

bool EquivalenceDiscoverer::handleEquivalence(SATLiteral l1, SATLiteral l2, UnitList*& eqAcc)
{
  CALL("EquivalenceDiscoverer::handleEquivalence");

  ASS_NEQ(l1.var(), l2.var());

  Literal* fl1 = getFOLit(l1);
  Literal* fl2 = getFOLit(l2);

  static DHMap<unsigned,unsigned> varSorts;
  varSorts.reset();
  if(!SortHelper::areSortsValid(fl1, varSorts) || !SortHelper::areSortsValid(fl2, varSorts)) {
    return false;
  }

  Formula* eqForm = new BinaryFormula(IFF, new AtomicFormula(fl1), new AtomicFormula(fl2));
  Formula::VarList* freeVars = eqForm->freeVariables();
  if(freeVars) {
    eqForm = new QuantifiedFormula(FORALL, freeVars, eqForm);
  }

  Inference* inf = getEquivInference(l1, l2);
  FormulaUnit* fu = new FormulaUnit(eqForm, inf, Unit::AXIOM);
  UnitList::push(fu, eqAcc);

  if(!_useISS) {
    static SATLiteralStack slits;
    slits.reset();
    slits.push(l1);
    slits.push(l2.opposite());
    SATClause* scl1 = SATClause::fromStack(slits);
    scl1->setInference(new FOConversionInference(UnitSpec(fu)));

    slits.reset();
    slits.push(l1.opposite());
    slits.push(l2);
    SATClause* scl2 = SATClause::fromStack(slits);
    scl2->setInference(new FOConversionInference(UnitSpec(fu)));

    _solver->addClauses(
	pvi( getConcatenatedIterator(getSingletonIterator(scl1),getSingletonIterator(scl2)) ), true);
  }

  LOG_UNIT("pp_ed_eq",fu);
  TRACE("pp_ed_eq_prems",
	UnitSpecIterator uit = InferenceStore::instance()->getParents(UnitSpec(fu));
	while(uit.hasNext()) {
	  UnitSpec p = uit.next();
	  TRACE_OUTPUT_UNIT("pp_ed_eq_prems",p.unit());
	}
  );

  return true;
}

bool EquivalenceDiscoverer::areEquivalent(SATLiteral l1, SATLiteral l2)
{
  CALL("EquivalenceDiscoverer::areEquivalent");
  ASS_NEQ(l1,l2);
  ASS(!_solver->hasAssumptions());
  ASS_NEQ(_solver->getStatus(),SATSolver::UNSATISFIABLE);

  unsigned v1 = l1.var();
  unsigned v2 = l2.var();
  bool eqPol = l1.polarity()==l2.polarity();

  bool v1InitAsgn;
  bool v2InitAsgn;
  if(!_initialAssignment.find(v1, v1InitAsgn) || !_initialAssignment.find(v2, v2InitAsgn)) {
    return false;
  }
  if((v1InitAsgn==v2InitAsgn)!=eqPol) {
    return false;
  }

  bool firstAssumptionPropOnly = true;
//  bool firstAssumptionPropOnly = _onlyPropEqCheck;

  LOG("pp_ed_asm","asserted l1: "<<l1);
  _solver->addAssumption(l1, firstAssumptionPropOnly);
  LOG("pp_ed_asm","result for l1: "<<_solver->getStatus());
  LOG("pp_ed_asm","asserted ~l2: "<<l2.opposite());
  _solver->addAssumption(l2.opposite(), _satConflictCountLimit);

  SATSolver::Status status = _solver->getStatus();
  LOG("pp_ed_asm","result for ~(l1=>l2): "<<status);
  _solver->retractAllAssumptions();
  LOG("pp_ed_asm","assumptions retracted");

  if(status!=SATSolver::UNSATISFIABLE) {
    return false;
  }
  LOG("pp_ed_asm","asserted ~l1: "<<l1.opposite());
  _solver->addAssumption(l1.opposite(), firstAssumptionPropOnly);
  LOG("pp_ed_asm","result for ~l1: "<<_solver->getStatus());
  LOG("pp_ed_asm","asserted l2: "<<l2);
  _solver->addAssumption(l2, _satConflictCountLimit);

  status = _solver->getStatus();
  LOG("pp_ed_asm","result for ~(l2=>l1): "<<status);
  _solver->retractAllAssumptions();
  LOG("pp_ed_asm","assumptions retracted");

  return status==SATSolver::UNSATISFIABLE;
}

UnitList* EquivalenceDiscoverer::getEquivalences(UnitList* units, const Options* opts)
{
  CALL("EquivalenceDiscoverer::getEquivalences");

  Options prepOpts;
  if(opts) { prepOpts = *opts; }
  prepOpts.setPredicateEquivalenceDiscovery(false);

  Problem prb(units->copy());

  Preprocess prepr(prepOpts);
  prepr.preprocess(prb);
  //TODO: we will leak the results of this preprocessing iteration

  return getEquivalences(prb.clauseIterator());
}

//////////////////////////////////////
// EquivalenceDiscoveringTransformer
//

EquivalenceDiscoveringTransformer::EquivalenceDiscoveringTransformer(const Options& opts)
 : _opts(opts)
{

}

bool EquivalenceDiscoveringTransformer::apply(Problem& prb)
{
  CALL("EquivalenceDiscoveringTransformer::apply(Problem&)");

  if(apply(prb.units())) {
    prb.invalidateProperty();
    return true;
  }
  return false;
}

bool EquivalenceDiscoveringTransformer::apply(UnitList*& units)
{
  CALL("EquivalenceDiscoveringTransformer::apply(UnitList*&)");

  EquivalenceDiscoverer eqd(true, _opts.predicateEquivalenceDiscoverySatConflictLimit(), !_opts.predicateEquivalenceDiscoveryAllAtoms());
  UnitList* equivs = eqd.getEquivalences(units, &_opts);
  if(!equivs) {
    return false;
  }

  units = UnitList::concat(equivs, units);

  PDInliner inl;
  inl.apply(units, true);
  return true;
}



}
