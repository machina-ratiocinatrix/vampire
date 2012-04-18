/**
 * @file BDD.hpp
 * Defines classes BDD and BDDNode for binary decision diagrams
 */

#ifndef __BDD__
#define __BDD__

#include <iosfwd>
#include <string>

#include "Forwards.hpp"
#include "Lib/Allocator.hpp"
#include "Lib/Array.hpp"
#include "Lib/DHMap.hpp"
#include "Lib/Hash.hpp"
#include "Lib/Int.hpp"
#include "Lib/List.hpp"
#include "Lib/Set.hpp"
#include "Lib/SkipList.hpp"
#include "Lib/Stack.hpp"

#include "Kernel/Signature.hpp"

#define BDD_PREDICATE_PREFIX "$bdd"

namespace Kernel {

using namespace std;
using namespace Lib;
using namespace SAT;

class BDDConjunction;

/**
 * A class of objects representing nodes in binary decision diagrams.
 */
class BDDNode
{
public:
  CLASS_NAME(BDDNode);
  USE_ALLOCATOR(BDDNode);

  unsigned depth() const { return _depth; }
  bool isConst() const { return _var==0; }
  bool isTrue() const;
  bool isFalse() const;
  bool isAtomic() const
  { return !isConst() && getPos()->isConst() && getNeg()->isConst(); }

  unsigned getVar() const { ASS(!isConst()); return _var; }
  BDDNode* getPos() const { ASS(!isConst()); return _pos; }
  BDDNode* getNeg() const { ASS(!isConst()); return _neg; }
private:
  BDDNode() : _refuted(false) {}
  BDDNode(unsigned var, BDDNode* pos, BDDNode* neg) :
      _refuted(false), _var(var), _pos(pos), _neg(neg) {}

  bool _refuted : 1;
  unsigned _var : 31;
  unsigned _depth;

  BDDNode* _pos;
  BDDNode* _neg;

  friend class BDD;
  friend class BDDConjunction;
  friend class BDDClausifier;
  friend class Shell::LaTeX;
};

/**
 * A class of binary decision diagrams.
 *
 * The BDD object is a singleton, the instance can be obtained by
 * the @b instance() method.
 */
class BDD
{
public:
  static BDD* instance();

  BDD();
  ~BDD();

  /** Return an unused BDD variable number */
  int getNewVar() { return _newVar++; }
  int getNewVar(unsigned pred);

  string getPropositionalPredicateName(int var);
  bool getNiceName(int var, string& res);
  Signature::Symbol* getSymbol(int var);

  /** Return a BDD node representing true formula */
  BDDNode* getTrue() { return &_trueNode; }
  /** Return a BDD node representing false formula */
  BDDNode* getFalse() { return &_falseNode; }

  BDDNode* getAtomic(int varNum, bool positive);

  BDDNode* conjunction(BDDNode* n1, BDDNode* n2);
  BDDNode* disjunction(BDDNode* n1, BDDNode* n2);
  BDDNode* xOrNonY(BDDNode* x, BDDNode* y);
  /** Return a BDD node of negation of @b n */
  BDDNode* negation(BDDNode* n)
  { return xOrNonY(getFalse(), n); }
  BDDNode* assignValue(BDDNode* n, unsigned var, bool value);

  bool isXOrNonYConstant(BDDNode* x, BDDNode* y, bool resValue);

  /** Return @b true iff @b node represents a true formula */
  bool isTrue(const BDDNode* node) { return node==getTrue(); }
  /** Return @b true iff @b node represents a false formula */
  bool isFalse(const BDDNode* node) { return node==getFalse(); }
  /** Return @b true iff @b node represents either a false or a true formula */
  bool isConstant(const BDDNode* node) { return node->_var==0; }

  bool parseAtomic(BDDNode* node, unsigned& var, bool& positive);

  bool findTrivial(BDDNode* n, bool& areImplied, Stack<BDDNode*>& acc);

  static bool equals(const BDDNode* n1,const BDDNode* n2);
  static unsigned hash(const BDDNode* n);

  string toString(BDDNode* node);
  string toTPTPString(BDDNode* node, string bddPrefix);
  string toTPTPString(BDDNode* node);

  Formula* toFormula(BDDNode* node);

  string getDefinition(BDDNode* node);
  string getName(BDDNode* node);
  TermList getConstant(BDDNode* node);

  void allowDefinitionOutput(bool allow);

  void markRefuted(BDDNode* n) { n->_refuted=true; }
  bool isRefuted(BDDNode* n) { return n->_refuted; }

private:
  void outputDefinition(string def);
  string introduceName(BDDNode* node, string definition);

  BDDNode* getNode(int varNum, BDDNode* pos, BDDNode* neg);

  template<class BinBoolFn>
  BDDNode* getBinaryFnResult(BDDNode* n1, BDDNode* n2, BinBoolFn fn);

  template<bool ResValue, class BinBoolFn>
  bool hasConstantResult(BDDNode* n1, BDDNode* n2, BinBoolFn fn);

  enum Operation
  {
    CONJUNCTION,
    DISJUNCTION,
    X_OR_NON_Y,
    ASSIGNMENT
  };

  /**
   * A functor used by @b getBinaryFnResult to compute the conjunction of two BDDs,
   * and by @b hasConstantResult to check whether the conjunction of two BDDs is
   * either a true or a false formula.
   */
  struct ConjunctionFn
  {
    static const Operation op=CONJUNCTION;
    static const bool commutative=true;

    ConjunctionFn(BDD* parent) : _parent(parent) {}
    inline BDDNode* operator()(BDDNode* n1, BDDNode* n2);
    BDD* _parent;
  };

  /**
   * A functor used by @b getBinaryFnResult to compute the disjunction of two BDDs,
   * and by @b hasConstantResult to check whether the disjunction of two BDDs is
   * either a true or a false formula.
   */
  struct DisjunctionFn
  {
    static const Operation op=DISJUNCTION;
    static const bool commutative=true;

    DisjunctionFn(BDD* parent) : _parent(parent) {}
    inline BDDNode* operator()(BDDNode* n1, BDDNode* n2);
    BDD* _parent;
  };

  /**
   * A functor used by @b getBinaryFnResult to compute X | ~Y of two BDDs X and Y,
   * and by @b hasConstantResult to check whether X | ~Y of two BDDs X and Y is
   * either a true or a false formula.
   */
  struct XOrNonYFn
  {
    static const Operation op=X_OR_NON_Y;
    static const bool commutative=false;

    XOrNonYFn(BDD* parent) : _parent(parent) {}
    inline BDDNode* operator()(BDDNode* n1, BDDNode* n2);
    BDD* _parent;
  };

  /**
   * A functor used by @b getBinaryFnResult to compute result of assigning a constant
   * to a variable. LHS must be a BDD representing an atomic variable, RHS is the BDD
   * that the assignment is being performed on.
   */
  struct AssignFn
  {
    static const Operation op=ASSIGNMENT;
    static const bool commutative=false;

    AssignFn(BDD* parent) : _parent(parent) {}
    inline BDDNode* operator()(BDDNode* n1, BDDNode* n2);
    BDD* _parent;
  };




  /** BDD node representing the true formula */
  BDDNode _trueNode;
  /** BDD node representing the false formula */
  BDDNode _falseNode;

  /** Type that stores the set of all non-constant BDD nodes */
  typedef Set<BDDNode*,BDD> NodeSet;
  /** The set storing all nodes */
  NodeSet _nodes;

  /**
   * Predicate symbols corresponding to BDD variables
   *
   * Not all BDD variables must have a corresponding predicate.
   */
  DHMap<int, unsigned> _predicateSymbols;

  DHMap<BDDNode*,string> _nodeNames;
  DHMap<BDDNode*,TermList> _nodeConstants;
  unsigned _bddEvalPredicate;

  int _nextNodeNum;
  bool _allowDefinitionOutput;
  Stack<string> _postponedDefinitions;

  /** The next unused BDD variable */
  int _newVar;
};

};

#endif /* __BDD__ */
