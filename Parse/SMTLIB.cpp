/**
 * @file SMTLIB.cpp
 * Implements class SMTLIB.
 */

#include <climits>
#include <fstream>

#include "Lib/Environment.hpp"
#include "Lib/NameArray.hpp"
#include "Lib/StringUtils.hpp"

#include "Kernel/ColorHelper.hpp"
#include "Kernel/Formula.hpp"
#include "Kernel/FormulaUnit.hpp"
#include "Kernel/Inference.hpp"
#include "Kernel/Signature.hpp"
#include "Kernel/SortHelper.hpp"

#include "Shell/AIGInliner.hpp"
#include "Shell/AIGCompressor.hpp"
#include "Shell/LispLexer.hpp"
#include "Shell/Options.hpp"

#include "SMTLIB.hpp"

namespace Parse
{

SMTLIB::SMTLIB(const Options& opts, Mode mode)
: _lispFormula(0),
  _definitions(0),
  _formulas(0),
  _mode(mode),
  _treatIntsAsReals(opts.smtlibConsiderIntsReal()),
  _defIntroThreshold(opts.aigDefinitionIntroductionThreshold()),
  _fletAsDefinition(opts.smtlibFletAsDefinition()),
  _introducedSymbolColor(COLOR_TRANSPARENT)
{
  CALL("SMTLIB::SMTLIB");

#if VDEBUG
  _haveParsed = false;
#endif
}

void SMTLIB::parse(istream& str)
{
  CALL("SMTLIB::parse(istream&)");

  LispLexer lex(str);
  LispParser lpar(lex);
  LExpr* expr = lpar.parse();

  LispListReader eRdr(expr);
  parse(eRdr.readListExpr());
  eRdr.acceptEOL();
}

/**
 * @param bench lisp list having atom "benchmark" as the first element
 */
void SMTLIB::parse(LExpr* bench)
{
  CALL("SMTLIB::parse(LExpr*)");
  ASS(!_haveParsed);
  ASS(bench->isList());
#if VDEBUG
  _haveParsed = true;
#endif

  readBenchmark(bench->list);

  if(_mode==READ_BENCHMARK) { return; }

  doSortDeclarations();

  if(_mode==DECLARE_SORTS) { return; }

  doFunctionDeclarations();

  if(_mode==DECLARE_SYMBOLS) { return; }
  ASS(_mode==BUILD_FORMULA || _mode==INTRODUCE_NAMES);

  buildFormula();
}

void SMTLIB::readBenchmark(LExprList* bench)
{
  LispListReader bRdr(bench);
  bRdr.acceptAtom("benchmark");
  _benchName = bRdr.readAtom();

  while(bRdr.hasNext()) {
    if(bRdr.tryAcceptAtom(":status")) {
      _statusStr = bRdr.readAtom();
    }
    else if(bRdr.tryAcceptAtom(":source")) {
      if(!bRdr.tryAcceptCurlyBrackets()) {
	bRdr.acceptAtom();
      }
    }
    else if(bRdr.tryAcceptAtom(":extrasorts")) {
      LispListReader declsListRdr(bRdr.readList());
      while(declsListRdr.hasNext()) {
	readSort(declsListRdr.readAtom());
      }
    }
    else if(bRdr.tryAcceptAtom(":extrafuns")) {
      LispListReader declsListRdr(bRdr.readList());
      while(declsListRdr.hasNext()) {
	readFunction(declsListRdr.readList());
      }
    }
    else if(bRdr.tryAcceptAtom(":extrapreds")) {
      LispListReader declsListRdr(bRdr.readList());
      while(declsListRdr.hasNext()) {
	readPredicate(declsListRdr.readList());
      }
    }
    else if(bRdr.tryAcceptAtom(":formula")) {
      if(_lispFormula) {
	USER_ERROR("two :formula elements in one benchmark");
      }
      _lispFormula = bRdr.readNext();
    }
    else {
      //this will always give an error as we have bRdr.hasNext() set to true
      bRdr.acceptEOL();
    }
  }
}

void SMTLIB::readFunction(LExprList* decl)
{
  CALL("SMTLIB::declareFunction");

  LispListReader dRdr(decl);
  string name = dRdr.readAtom();

  static Stack<string> argSorts;
  argSorts.reset();
  argSorts.push(dRdr.readAtom());
  while(dRdr.hasNext()) {
    argSorts.push(dRdr.readAtom());
  }

  string domainSort = argSorts.pop();

  _funcs.push(FunctionInfo(name, argSorts, domainSort));
}

void SMTLIB::readPredicate(LExprList* decl)
{
  CALL("SMTLIB::declarePredicate");

  LispListReader dRdr(decl);
  string name = dRdr.readAtom();

  static Stack<string> argSorts;
  argSorts.reset();
  while(dRdr.hasNext()) {
    argSorts.push(dRdr.readAtom());
  }

  _funcs.push(FunctionInfo(name, argSorts, "$o"));
}

unsigned SMTLIB::getSort(string name)
{
  CALL("SMTLIB::getSort");

  if(name=="Real") {
    return Sorts::SRT_REAL;
  }
  else if(name=="Int") {
    return _treatIntsAsReals ? Sorts::SRT_REAL : Sorts::SRT_INTEGER;
  }

  unsigned idx;
  if(!env.sorts->findSort(name, idx)) {
    USER_ERROR("undeclared sort: "+name);
  }
  return idx;
}

void SMTLIB::doSortDeclarations()
{
  CALL("SMTLIB::doSortDeclarations");

  Stack<string>::Iterator srtIt(_userSorts);
  while(srtIt.hasNext()) {
    string sortName = srtIt.next();
    env.sorts->addSort(sortName);
  }
}

BaseType* SMTLIB::getSymbolType(const FunctionInfo& fnInfo)
{
  CALL("SMTLIB::getSymbolType");

  unsigned arity = fnInfo.argSorts.size();
  unsigned rangeSort = getSort(fnInfo.rangeSort);

  static Stack<unsigned> argSorts;
  argSorts.reset();

  Stack<string>::BottomFirstIterator argSortIt(fnInfo.argSorts);
  while(argSortIt.hasNext()) {
    string argSortName = argSortIt.next();
    argSorts.push(getSort(argSortName));
  }

  BaseType* type = BaseType::makeType(arity, argSorts.begin(), rangeSort);
  return type;
}

void SMTLIB::doFunctionDeclarations()
{
  CALL("SMTLIB::doFunctionDeclarations");

  unsigned funCnt = _funcs.size();
  for(unsigned i=0; i<funCnt; i++) {
    FunctionInfo& fnInfo = _funcs[i];

    unsigned arity = fnInfo.argSorts.size();

    BaseType* type = getSymbolType(fnInfo);
    bool isPred = !type->isFunctionType();

    unsigned symNum;
    Signature::Symbol* sym;
    if(isPred) {
      bool added;
      symNum = env.signature->addPredicate(fnInfo.name, arity, added);
      sym = env.signature->getPredicate(symNum);
      if(added) {
	sym->setType(type);
      }
      else {
	if((*sym->predType())!=(*type)) {
	  USER_ERROR("incompatible type for predicate "+fnInfo.name);
	}
      }
    }
    else {
      bool added;
      symNum = env.signature->addFunction(fnInfo.name, arity, added);
      sym = env.signature->getFunction(symNum);
      if(added) {
	sym->setType(type);
      }
      else {
	if((*sym->fnType())!=(*type)) {
	  USER_ERROR("incompatible type for function "+fnInfo.name);
	}
      }
    }
  }
}

///////////////////////
// Formula building
//

const char * SMTLIB::s_formulaSymbolNameStrings[] = {
    "<",
    "<=",
    "=",
    ">",
    ">=",
    "and",
    "exists",
    "flet",
    "forall",
    "if_then_else",
    "iff",
    "implies",
    "let",
    "not",
    "or",
    "xor"
};

const char * SMTLIB::s_termSymbolNameStrings[] = {
    "*",
    "+",
    "-",
    "ite",
    "~"
};

SMTLIB::FormulaSymbol SMTLIB::getFormulaSymbol(string str)
{
  CALL("SMTLIB::getFormulaSymbol");

  static NameArray formulaSymbolNames(s_formulaSymbolNameStrings, sizeof(s_formulaSymbolNameStrings)/sizeof(char*));
  ASS_EQ(formulaSymbolNames.length, FS_USER_PRED_SYMBOL);

  int res = formulaSymbolNames.tryToFind(str.c_str());
  if(res==-1) {
    return FS_USER_PRED_SYMBOL;
  }
  return static_cast<FormulaSymbol>(res);
}

SMTLIB::TermSymbol SMTLIB::getTermSymbol(string str)
{
  CALL("SMTLIB::getTermSymbol");

  static NameArray termSymbolNames(s_termSymbolNameStrings, sizeof(s_termSymbolNameStrings)/sizeof(char*));
  ASS_EQ(termSymbolNames.length, TS_USER_FUNCTION);

  int res = termSymbolNames.tryToFind(str.c_str());
  if(res==-1) {
    return TS_USER_FUNCTION;
  }
  return static_cast<TermSymbol>(res);
}

/**
 * If 0 is returned, there is no mandatory argument count
 */
unsigned SMTLIB::getMandatoryConnectiveArgCnt(FormulaSymbol fsym)
{
  CALL("SMTLIB::getConnectiveArgCnt");

  switch(fsym) {
  case FS_AND:
  case FS_OR:
    return 0;
  case FS_NOT:
    return 1;
  case FS_IFF:
  case FS_IMPLIES:
  case FS_XOR:
    return 2;
  case FS_IF_THEN_ELSE:
    return 3;
  default:
    ASSERTION_VIOLATION;
  }
}

//unsigned SMTLIB::getPredSymbolArity(FormulaSymbol fsym, string str)
//{
//  switch(fsym) {
//  case FS_EQ:
//    return 2;
//  case FS_USER_PRED_SYMBOL:
//    return env.signature->pre
//  }
//}

unsigned SMTLIB::getSort(TermList t)
{
  CALL("SMTLIB::readTermFromAtom");

  unsigned res;
  TermList mvar;
  if(!SortHelper::getResultSortOrMasterVariable(t, res, mvar)) {
    ASS(mvar.isVar());
    unsigned varIdx = mvar.var();
    ASS_L(varIdx, _varSorts.size());
    res = _varSorts[varIdx];
  }
  return res;
}

void SMTLIB::ensureArgumentSorts(bool pred, unsigned symNum, TermList* args)
{
  CALL("SMTLIB::ensureArgumentSorts");

  BaseType* type;
  if(pred) {
    type = env.signature->getPredicate(symNum)->predType();
  }
  else {
    type = env.signature->getFunction(symNum)->fnType();
  }
  unsigned arity = type->arity();
  for(unsigned i=0; i<arity; i++) {
    if(type->arg(i)!=getSort(args[i])) {
      USER_ERROR("argument sort mismatch: "+args[i].toString());
    }
  }
}

TermList SMTLIB::readTermFromAtom(string str)
{
  CALL("SMTLIB::readTermFromAtom");

  if(str[0]=='?') {
    TermList res;
    if(!_termVars.find(str, res)) {
      USER_ERROR("undefined term variable: "+str);
    }
    return res;
  }

  if(str[0]>='0' && str[0]<='9') {
    if(!_treatIntsAsReals && StringUtils::isPositiveInteger(str)) {
      return TermList(Theory::instance()->representIntegerConstant(str));
    }
    else if(StringUtils::isPositiveDecimal(str)) {
      return TermList(Theory::instance()->representRealConstant(str));
    }
    else {
      USER_ERROR("invalid base term: "+str);
    }
  }

  if(!env.signature->functionExists(str, 0)) {
    USER_ERROR("undeclared constant: "+str);
  }
  return TermList(Term::createConstant(str));
}

bool SMTLIB::tryReadTermIte(LExpr* e, TermList& res)
{
  CALL("SMTLIB::tryReadTermIte");

  LispListReader rdr(e);
  rdr.acceptAtom("ite");

  bool gotAll = true;
  Formula* cond;
  TermList thenBranch;
  TermList elseBranch;
  gotAll |= tryGetArgumentFormula(e, rdr.readNext(), cond);
  gotAll |= tryGetArgumentTerm(e, rdr.readNext(), thenBranch);
  gotAll |= tryGetArgumentTerm(e, rdr.readNext(), elseBranch);
  if(!gotAll) {
    return false;
  }
  res = TermList(Term::createTermITE(cond, thenBranch, elseBranch));
  return true;
}

/**
 * Assume all the remaining elements in @c rdr to be term expressions
 * and attempt to obtain their TermList representation to be put into
 * the @c args stack. If successful, return true, otherwise return false,
 * put an empty TermList in place of arguments that could not have been
 * obtained and schedule those arguments for processing on the _todo list
 * (by a call to the tryGetArgumentTerm() function).
 */
bool SMTLIB::readTermArgs(LExpr* parent, LispListReader& rdr, TermStack& args)
{
  CALL("SMTLIB::readTermArgs");
  ASS(args.isEmpty());

  bool someArgsUnevaluated = false;

  while(rdr.hasNext()) {
    TermList arg;
    string atomArgStr;
    if(rdr.tryReadAtom(atomArgStr)) {
      arg = readTermFromAtom(atomArgStr);
    }
    else {
      LExpr* argExpr = rdr.readListExpr();
      if(!tryGetArgumentTerm(parent, argExpr, arg)) {
        someArgsUnevaluated = true;
      }
    }
    args.push(arg);
  }
  return !someArgsUnevaluated;
}

Interpretation SMTLIB::getFormulaSymbolInterpretation(FormulaSymbol fs, unsigned firstArgSort)
{
  CALL("SMTLIB::getFormulaSymbolInterpretation");

  Interpretation res = Theory::INVALID_INTERPRETATION;
  switch(fs) {
  case FS_LESS:
    switch(firstArgSort) {
    case Sorts::SRT_INTEGER:
	res = Theory::INT_LESS;
      break;
    case Sorts::SRT_REAL:
	res = Theory::REAL_LESS;
      break;
    default:
      break;
    }
    break;
  case FS_LESS_EQ:
    switch(firstArgSort) {
    case Sorts::SRT_INTEGER:
	res = Theory::INT_LESS_EQUAL;
      break;
    case Sorts::SRT_REAL:
      res = Theory::REAL_LESS_EQUAL;
      break;
    default:
      break;
    }
    break;
  case FS_GREATER:
    switch(firstArgSort) {
    case Sorts::SRT_INTEGER:
      res = Theory::INT_GREATER;
      break;
    case Sorts::SRT_REAL:
      res = Theory::REAL_GREATER;
      break;
    default:
      break;
    }
    break;
  case FS_GREATER_EQ:
    switch(firstArgSort) {
    case Sorts::SRT_INTEGER:
      res = Theory::INT_GREATER_EQUAL;
      break;
    case Sorts::SRT_REAL:
      res = Theory::REAL_GREATER_EQUAL;
      break;
    default:
      break;
    }
    break;

  default:
    ASSERTION_VIOLATION;
  }
  if(res==Theory::INVALID_INTERPRETATION) {
    USER_ERROR("invalid sort "+env.sorts->sortName(firstArgSort)+" for interpretation "+string(s_formulaSymbolNameStrings[fs]));
  }
  return res;
}

Interpretation SMTLIB::getTermSymbolInterpretation(TermSymbol ts, unsigned firstArgSort)
{
  CALL("SMTLIB::getTermSymbolInterpretation");

  Interpretation res = Theory::INVALID_INTERPRETATION;
  switch(ts) {
  case TS_MINUS:
    switch(firstArgSort) {
    case Sorts::SRT_INTEGER:
	res = Theory::INT_MINUS;
      break;
    case Sorts::SRT_REAL:
	res = Theory::REAL_MINUS;
      break;
    default:
      break;
    }
    break;
  case TS_PLUS:
    switch(firstArgSort) {
    case Sorts::SRT_INTEGER:
	res = Theory::INT_PLUS;
      break;
    case Sorts::SRT_REAL:
      res = Theory::REAL_PLUS;
      break;
    default:
      break;
    }
    break;
  case TS_MULTIPLY:
    switch(firstArgSort) {
    case Sorts::SRT_INTEGER:
      res = Theory::INT_MULTIPLY;
      break;
    case Sorts::SRT_REAL:
      res = Theory::REAL_MULTIPLY;
      break;
    default:
      break;
    }
    break;
  case TS_UMINUS:
    switch(firstArgSort) {
    case Sorts::SRT_INTEGER:
      res = Theory::INT_UNARY_MINUS;
      break;
    case Sorts::SRT_REAL:
      res = Theory::REAL_UNARY_MINUS;
      break;
    default:
      break;
    }
    break;

  default:
    ASSERTION_VIOLATION;
  }
  if(res==Theory::INVALID_INTERPRETATION) {
    USER_ERROR("invalid sort "+env.sorts->sortName(firstArgSort)+" for interpretation "+string(s_termSymbolNameStrings[ts]));
  }
  return res;
}

bool SMTLIB::tryReadTerm(LExpr* e, TermList& res)
{
  CALL("SMTLIB::tryReadTerm");

  if(e->isAtom()) {
    res = readTermFromAtom(e->str);
    return true;
  }

  LispListReader rdr(e);
  string fnName = rdr.readAtom();
  TermSymbol ts = getTermSymbol(fnName);

  if(ts==TS_ITE) {
    return tryReadTermIte(e, res);
  }

  static TermStack args;
  args.reset();

  if(!readTermArgs(e, rdr, args)) {
    return false;
  }

  unsigned arity = args.size();
  unsigned fnNum;

  if(ts==TS_USER_FUNCTION) {
    if(!env.signature->functionExists(fnName, arity)) {
      USER_ERROR("undeclared function: "+fnName+"/"+Int::toString(arity));
    }
    fnNum = env.signature->addFunction(fnName, arity);
  }
  else {
    if(arity==0) {
      USER_ERROR("interpreted function with zero arity: "+fnName);
    }
    unsigned firstArgSort = getSort(args[0]);
    Interpretation itp = getTermSymbolInterpretation(ts, firstArgSort);
    if(Theory::instance()->getArity(itp)!=args.size()) {
      USER_ERROR("invalid function arity: "+fnName);
    }
    fnNum = Theory::instance()->getFnNum(itp);
  }

  ASS_EQ(env.signature->functionArity(fnNum), args.size());
  ensureArgumentSorts(false, fnNum, args.begin());
  res = TermList(Term::create(fnNum, arity, args.begin()));
  return true;
}

bool SMTLIB::tryReadNonPropAtom(FormulaSymbol fsym, LExpr* e, Literal*& res)
{
  CALL("SMTLIB::tryReadNonPropAtom");

  LispListReader rdr(e);
  string predName = rdr.readAtom();

  static TermStack args;
  args.reset();

  if(!readTermArgs(e, rdr, args)) {
    return false;
  }

  if(fsym==FS_EQ) {
    if(args.size()!=2) {
      USER_ERROR("equality requires two arguments: "+e->toString());
    }
    unsigned srt = getSort(args[0]);
    if(srt!=getSort(args[1])) {
      USER_ERROR("equality argument sort mismatch: "+e->toString());
    }
    res = Literal::createEquality(true, args[0], args[1], srt);
    return true;
  }


  unsigned arity = args.size();
  unsigned predNum;

  if(fsym==FS_USER_PRED_SYMBOL) {
    if(!env.signature->predicateExists(predName, arity)) {
      USER_ERROR("undeclared predicate: "+predName+"/"+Int::toString(arity));
    }
    predNum = env.signature->addPredicate(predName, arity);
  }
  else {
    if(arity==0) {
      USER_ERROR("interpreted predicate with zero arity: "+predName);
    }
    unsigned firstArgSort = getSort(args[0]);
    Interpretation itp = getFormulaSymbolInterpretation(fsym, firstArgSort);
    if(Theory::instance()->getArity(itp)!=args.size()) {
      USER_ERROR("invalid predicate arity: "+predName);
    }
    predNum = Theory::instance()->getPredNum(itp);
  }

  ASS_EQ(env.signature->predicateArity(predNum), args.size());
  ensureArgumentSorts(true, predNum, args.begin());
  res = Literal::create(predNum, arity, true, false, args.begin());
  return true;
}

Formula* SMTLIB::readFormulaFromAtom(string str)
{
  CALL("SMTLIB::readFormulaFromAtom");

  if(str=="true") {
    return Formula::trueFormula();
  }
  if(str=="false") {
    return Formula::falseFormula();
  }
  if(str[0]=='$') {
    Formula* res;
    if(!_formVars.find(str, res)) {
      USER_ERROR("undefined formula variable "+str);
    }
    return res;
  }
  if(str[0]=='?') {
    USER_ERROR("term variable where formula was expected: "+str);
  }
  if(!env.signature->predicateExists(str, 0)) {
    USER_ERROR("undeclared propositional predicate: "+str);
  }
  unsigned predNum = env.signature->addPredicate(str, 0);
  Literal* resLit = Literal::create(predNum, 0, true, false, 0);
  return new AtomicFormula(resLit);
}

bool SMTLIB::tryReadConnective(FormulaSymbol fsym, LExpr* e, Formula*& res)
{
  CALL("SMTLIB::tryReadConnective");

  LispListReader rdr(e);
  rdr.acceptAtom();

  bool someArgsUnevaluated = false;

  static FormulaStack argForms;
  argForms.reset();
  while(rdr.hasNext()) {
    LExpr* arg = rdr.readNext();
    Formula* form = 0;
    if(!tryGetArgumentFormula(e, arg, form)) {
      someArgsUnevaluated = true;
    }
    argForms.push(form);
  }
  if(someArgsUnevaluated) {
    return false;
  }
  if(argForms.isEmpty()) {
    USER_ERROR("conective with no arguments: "+e->toString());
  }
  unsigned mandArgCnt = getMandatoryConnectiveArgCnt(fsym);
  if(mandArgCnt && argForms.size()!=mandArgCnt) {
    USER_ERROR("invalid argument number: "+e->toString());
  }

  switch(fsym) {
  case FS_NOT:
    res = new NegatedFormula(argForms[0]);
    break;
  case FS_AND:
  case FS_OR:
  {
    FormulaList* argLst = 0;
    FormulaList::pushFromIterator(FormulaStack::Iterator(argForms), argLst);
    res = new JunctionFormula( (fsym==FS_AND) ? AND : OR, argLst);
    break;
  }
  case FS_IFF:
  case FS_IMPLIES:
  case FS_XOR:
  {
    Connective con = (fsym==FS_IFF) ? IFF : ((fsym==FS_IMPLIES) ? IMP : XOR);
    res = new BinaryFormula(con, argForms[0], argForms[1]);
    break;
  }
  case FS_IF_THEN_ELSE:
    res = new IteFormula(argForms[0], argForms[1], argForms[2]);
    break;
  default:
    ASSERTION_VIOLATION;
  }
  return true;
}

bool SMTLIB::tryReadQuantifier(bool univ, LExpr* e, Formula*& res)
{
  CALL("SMTLIB::tryReadQuantifier");

  LispListReader rdr(e);
  rdr.acceptAtom();

  static Stack<LExpr*> qExprs;
  qExprs.reset();
  qExprs.loadFromIterator(rdr);

  LExpr* subFormExpr = qExprs.pop();

  static Stack<string> varNames;
  varNames.reset();

  Stack<LExpr*>::BottomFirstIterator qvarExprIt(qExprs);
  while(qvarExprIt.hasNext()) {
    LispListReader qvarRdr(qvarExprIt.next());
    string varName = qvarRdr.readAtom();
    string sortName = qvarRdr.readAtom();
    qvarRdr.acceptEOL();

    if(varName[0]!='?') {
      USER_ERROR("term variable expected in quantifier: "+varName);
    }
    if(_entering) {
      //we're entering the node
      if(_termVars.find(varName)) {
	USER_ERROR("quantifying bound variable: "+varName);
      }
      unsigned varIdx = _nextQuantVar++;
      unsigned sort = getSort(sortName);
      _termVars.insert(varName, TermList(varIdx, false));
      ASS_EQ(_varSorts.size(), varIdx);
      _varSorts.push(sort);
    }
    ASS(_termVars.find(varName));
    ASS(_termVars.get(varName).isVar());
    varNames.push(varName);
  }

  Formula* subForm;

  ASS_EQ(_forms.find(subFormExpr), !_entering);
  if(!tryGetArgumentFormula(e, subFormExpr, subForm)) {
    ASS(_entering);
    return false;
  }

  Formula::VarList* qvars = 0;
  Stack<string>::Iterator vnameIt(varNames);
  while(vnameIt.hasNext()) {
    string varName = vnameIt.next();
    unsigned varIdx = _termVars.get(varName).var();
    Formula::VarList::push(varIdx, qvars);
    ALWAYS(_termVars.remove(varName));
  }

  res = new QuantifiedFormula(univ ? FORALL : EXISTS, qvars, subForm);
  return true;
}

bool SMTLIB::tryReadFlet(LExpr* e, Formula*& res)
{
  CALL("SMTLIB::tryReadFlet");

  LispListReader rdr(e);

  rdr.acceptAtom("flet");
  LispListReader defRdr(rdr.readList());
  string varName = defRdr.readAtom();

  if(varName[0]!='$') {
    USER_ERROR("invalid formula variable name: "+varName);
  }

  if(_entering && _formVars.find(varName)) {
    USER_ERROR("flet binds a formula variable that is already bound: "+varName);
  }

  LExpr* varRhsExpr = defRdr.readNext();
  defRdr.acceptEOL();

  Formula* varRhs;
  if(!tryGetArgumentFormula(e, varRhsExpr, varRhs)) {
    ASS(_entering);
    //it is important that we return at this point and don't request the
    //formula for the flet body. The variable value has to be assigned
    //before we start processing that expression.
    return false;
  }
  ASS(!_entering);
  if(!_formVars.find(varName)) {
    if(_fletAsDefinition) {
      varRhs = nameFormula(varRhs, varName);
    }
    _formVars.insert(varName, varRhs);
  }

  LExpr* bodyExpr = rdr.readNext();
  if(!tryGetArgumentFormula(e, bodyExpr, res)) {
    return false;
  }

  ALWAYS(_formVars.remove(varName));
  return true;
}

bool SMTLIB::tryReadLet(LExpr* e, Formula*& res)
{
  CALL("SMTLIB::tryReadLet");

  LispListReader rdr(e);

  rdr.acceptAtom("let");
  LispListReader defRdr(rdr.readList());
  string varName = defRdr.readAtom();
  if(varName[0]!='?') {
    USER_ERROR("invalid term variable name: "+varName);
  }

  LExpr* varRhsExpr = defRdr.readNext();
  defRdr.acceptEOL();

  if(_entering && _termVars.find(varName)) {
    USER_ERROR("let binds a variable that is already bound: "+varName);
  }

  TermList varRhs;
  if(!tryGetArgumentTerm(e, varRhsExpr, varRhs)) {
    ASS(_entering);
    //it is important that we return at this point and don't request the
    //formula for the let body. The variable value has to be assigned
    //before we start processing that expression.
    return false;
  }
  ASS(!_entering);

  if(!_termVars.insert(varName, varRhs)) {
    //we're in the third call
  }

  LExpr* bodyExpr = rdr.readNext();
  if(!tryGetArgumentFormula(e, bodyExpr, res)) {
    return false;
  }

  ALWAYS(_termVars.remove(varName));
  return true;
}

bool SMTLIB::tryReadFormula(LExpr* e, Formula*& res)
{
  CALL("SMTLIB::tryReadFormula");

  if(e->isAtom()) {
    res = readFormulaFromAtom(e->str);
    return true;
  }

  LispListReader rdr(e);
  string sym = rdr.readAtom();
  FormulaSymbol fsym = getFormulaSymbol(sym);
  switch(fsym) {
  case FS_NOT:
  case FS_AND:
  case FS_IFF:
  case FS_IMPLIES:
  case FS_OR:
  case FS_XOR:
  case FS_IF_THEN_ELSE:
    return tryReadConnective(fsym, e, res);

  case FS_EXISTS:
  case FS_FORALL:
    return tryReadQuantifier(fsym==FS_FORALL, e, res);

  case FS_EQ:
  case FS_LESS:
  case FS_LESS_EQ:
  case FS_GREATER:
  case FS_GREATER_EQ:
  case FS_USER_PRED_SYMBOL:
  {
    Literal* lit;
    if(tryReadNonPropAtom(fsym, e, lit)) {
      res = new AtomicFormula(lit);
      return true;
    }
    return false;
  }

  case FS_FLET:
    return tryReadFlet(e, res);
  case FS_LET:
    return tryReadLet(e, res);
  default:
    ASSERTION_VIOLATION;
  }
}

bool SMTLIB::tryGetArgumentTerm(LExpr* parent, LExpr* argument, TermList& res)
{
  CALL("SMTLIB::tryGetArgumentTerm");
  ASS_EQ(parent,_current.first);

  if(_terms.find(argument, res)) {
    ASS(!_entering);
    return true;
  }
  requestSubexpressionProcessing(argument, false);
  return false;
}

bool SMTLIB::tryGetArgumentFormula(LExpr* parent, LExpr* argument, Formula*& res)
{
  CALL("SMTLIB::tryGetArgumentFormula");
  ASS_EQ(parent,_current.first);

  if(_forms.find(argument, res)) {
    ASS(!_entering);
    return true;
  }
  requestSubexpressionProcessing(argument, true);
  return false;
}

void SMTLIB::requestSubexpressionProcessing(LExpr* subExpr, bool formula)
{
  CALL("SMTLIB::requestSubexpressionProcessing");

  _todo.push(ToDoEntry(subExpr, formula));
  _todo.push(ToDoEntry(0, true));
}

void SMTLIB::buildFormula()
{
  CALL("SMTLIB::buildFormula");

  _nextQuantVar = 0;

  _todo.push(ToDoEntry(_lispFormula, true));
  _todo.push(ToDoEntry(0, true));

  while(_todo.isNonEmpty()) {
    _entering = false;
    if(_todo.top().first==0) {
      _entering = true;
      _todo.pop();
    }
    _current = _todo.top();
    ASS(_current.first);
    if(_current.second) {
      //we're processing a formula
      Formula* form;
      if(tryReadFormula(_current.first, form)) {
	ASS(_todo.top()==_current); //if reading succeeded, there aren't any new requests
	_todo.pop();
	ALWAYS(_forms.insert(_current.first, form));
      }
      else {
	ASS(_todo.top()!=_current); //if reading failed, there must be some new requests
      }
    }
    else {
      //we're processing a term
      TermList trm;
      if(tryReadTerm(_current.first, trm)) {
	ASS(_todo.top()==_current); //if reading succeeded, there aren't any new requests
	_todo.pop();
	ALWAYS(_terms.insert(_current.first, trm));
      }
      else {
	ASS(_todo.top()!=_current); //if reading failed, there must be some new requests
      }
    }
  }

  Formula* topForm;
  ALWAYS(_forms.find(_lispFormula, topForm));

  if(_mode>BUILD_FORMULA) {
    topForm = introduceAigNames(topForm);
  }
  FormulaUnit* fu = new FormulaUnit(topForm, new Inference(Inference::INPUT), Unit::CONJECTURE);

  ASS_EQ(_formulas,0);
  _formulas = _definitions->copy();
  UnitList::push(fu, _formulas);
}

Formula* SMTLIB::introduceAigNames(Formula* f)
{
  CALL("SMTLIB::introduceAigNames");

  f = AIGCompressingTransformer().apply(f);
  f = AIGDefinitionIntroducer(_defIntroThreshold).apply(f, _definitions);

  return f;
}

Formula* SMTLIB::nameFormula(Formula* f, string fletVarName)
{
  CALL("SMTLIB::nameFormula");

  Formula::VarList* freeVars = f->freeVariables();
  unsigned varCnt = freeVars->length();

  static DHMap<unsigned,unsigned> sorts;
  sorts.reset();

  SortHelper::collectVariableSorts(f, sorts);

  static Stack<unsigned> argSorts;
  static Stack<TermList> args;
  argSorts.reset();
  args.reset();

  Formula::VarList::Iterator vit(freeVars);
  while(vit.hasNext()) {
    unsigned var = vit.next();
    args.push(TermList(var, false));
    argSorts.push(sorts.get(var));
  }

  fletVarName = StringUtils::sanitizeSuffix(fletVarName);
  unsigned predNum = env.signature->addFreshPredicate(varCnt, "sP", fletVarName.c_str());
  BaseType* type = BaseType::makeType(varCnt, argSorts.begin(), Sorts::SRT_BOOL);

  Signature::Symbol* predSym = env.signature->getPredicate(predNum);
  predSym->setType(type);

//  Color symColor = ColorHelper::combine(_introducedSymbolColor, f->getColor());
//  if(symColor==COLOR_INVALID) {
//    USER_ERROR("invalid color in formula "+ f->toString());
//  }
//  if(symColor!=COLOR_TRANSPARENT) {
//    predSym->addColor(symColor);
//  }

  Literal* lhs = Literal::create(predNum, varCnt, true, false, args.begin());
  Formula* lhsF = new AtomicFormula(lhs);
  Formula* df = new BinaryFormula(IFF, lhsF, f);
  if(freeVars) {
    df = new QuantifiedFormula(FORALL, freeVars, df);
  }
  FormulaUnit* def = new FormulaUnit(df, new Inference(Inference::INPUT), Unit::AXIOM);
  UnitList::push(def, _definitions);
  return lhsF;
}

}
