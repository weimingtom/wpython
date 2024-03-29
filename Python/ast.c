/*
 * This file includes functions to transform a concrete syntax tree (CST) to
 * an abstract syntax tree (AST).  The main function is PyAST_FromNode().
 *
 */
#include "Python.h"
#include "Python-ast.h"
#include "grammar.h"
#include "node.h"
#include "pyarena.h"
#include "ast.h"
#include "token.h"
#include "parsetok.h"
#include "graminit.h"

#include <assert.h>

/* Data structure used internally */
struct compiling {
    char *c_encoding; /* source encoding */
    int c_future_unicode; /* __future__ unicode literals flag */
    int c_no_folding; /* no constant folding flag */
	/* enable strict constant folding (1/0 -> syntax error at compile time) */
    int c_strict_folding; 
    PyArena *c_arena; /* arena for allocating memeory */
    const char *c_filename; /* filename */
};

static asdl_seq *seq_for_testlist(struct compiling *, const node *,
								  enum _expr_const *constant);
static expr_ty ast_for_expr(struct compiling *, const node *);
static stmt_ty ast_for_stmt(struct compiling *, const node *);
static asdl_seq *ast_for_suite(struct compiling *, const node *);
static asdl_seq *ast_for_exprlist(struct compiling *, const node *,
                                  expr_context_ty);
static expr_ty ast_for_testlist(struct compiling *, const node *);
static stmt_ty ast_for_classdef(struct compiling *, const node *, asdl_seq *);
static expr_ty ast_for_testlist_gexp(struct compiling *, const node *);

/* Note different signature for ast_for_call */
static expr_ty ast_for_call(struct compiling *, const node *, expr_ty);

static PyObject *parsenumber(struct compiling *, const char *);
static PyObject *parsestr(struct compiling *, const char *);
static PyObject *parsestrplus(struct compiling *, const node *n);

#ifndef LINENO
#define LINENO(n)       ((n)->n_lineno)
#endif

static identifier
new_identifier(const char* n, PyArena *arena) {
    PyObject* id = PyString_InternFromString(n);
    if (id != NULL)
        PyArena_AddPyObject(arena, id);
    return id;
}

#define NEW_IDENTIFIER(n) new_identifier(STR(n), c->c_arena)

/* This routine provides an invalid object for the syntax error.
   The outermost routine must unpack this error and create the
   proper object.  We do this so that we don't have to pass
   the filename to everything function.

   XXX Maybe we should just pass the filename...
*/

static int
ast_error(const node *n, const char *errstr)
{
    PyObject *u = Py_BuildValue("zi", errstr, LINENO(n));
    if (!u)
        return 0;
    PyErr_SetObject(PyExc_SyntaxError, u);
    Py_DECREF(u);
    return 0;
}

static void
ast_error_finish(const char *filename)
{
    PyObject *type, *value, *tback, *errstr, *loc, *tmp;
    long lineno;

    assert(PyErr_Occurred());
    if (!PyErr_ExceptionMatches(PyExc_SyntaxError))
        return;

    PyErr_Fetch(&type, &value, &tback);
    errstr = PyTuple_GetItem(value, 0);
    if (!errstr)
        return;
    Py_INCREF(errstr);
    lineno = PyInt_AsLong(PyTuple_GetItem(value, 1));
    if (lineno == -1) {
        Py_DECREF(errstr);
        return;
    }
    Py_DECREF(value);

    loc = PyErr_ProgramText(filename, lineno);
    if (!loc) {
        Py_INCREF(Py_None);
        loc = Py_None;
    }
    tmp = Py_BuildValue("(zlOO)", filename, lineno, Py_None, loc);
    Py_DECREF(loc);
    if (!tmp) {
        Py_DECREF(errstr);
        return;
    }
    value = PyTuple_Pack(2, errstr, tmp);
    Py_DECREF(errstr);
    Py_DECREF(tmp);
    if (!value)
        return;
    PyErr_Restore(type, value, tback);
}

static int
ast_warn(struct compiling *c, const node *n, char *msg)
{
    if (PyErr_WarnExplicit(PyExc_SyntaxWarning, msg, c->c_filename, LINENO(n),
                           NULL, NULL) < 0) {
        /* if -Werr, change it to a SyntaxError */
        if (PyErr_Occurred() && PyErr_ExceptionMatches(PyExc_SyntaxWarning))
            ast_error(n, msg);
        return 0;
    }
    return 1;
}

static void*
ast_check_error(struct compiling *c, const node *n, char *msg)
{
	if (PyErr_Occurred())
		ast_error(n, msg);
    return NULL;
}

static int
forbidden_check(struct compiling *c, const node *n, const char *x)
{
    if (!strcmp(x, "None"))
        return ast_error(n, "assignment to None");
    if (Py_Py3kWarningFlag && !(strcmp(x, "True") && strcmp(x, "False")) &&
        !ast_warn(c, n, "assignment to True or False is forbidden in 3.x"))
        return 0;
    return 1;
}

/* num_stmts() returns number of contained statements.

   Use this routine to determine how big a sequence is needed for
   the statements in a parse tree.  Its raison d'etre is this bit of
   grammar:

   stmt: simple_stmt | compound_stmt
   simple_stmt: small_stmt (';' small_stmt)* [';'] NEWLINE

   A simple_stmt can contain multiple small_stmt elements joined
   by semicolons.  If the arg is a simple_stmt, the number of
   small_stmt elements is returned.
*/

static int
num_stmts(const node *n)
{
    int i, l;
    node *ch;

    switch (TYPE(n)) {
        case single_input:
            if (TYPE(CHILD(n, 0)) == NEWLINE)
                return 0;
            else
                return num_stmts(CHILD(n, 0));
        case file_input:
            l = 0;
            for (i = 0; i < NCH(n); i++) {
                ch = CHILD(n, i);
                if (TYPE(ch) == stmt)
                    l += num_stmts(ch);
            }
            return l;
        case stmt:
            return num_stmts(CHILD(n, 0));
        case compound_stmt:
            return 1;
        case simple_stmt:
            return NCH(n) / 2; /* Divide by 2 to remove count of semi-colons */
        case suite:
            if (NCH(n) == 1)
                return num_stmts(CHILD(n, 0));
            else {
                l = 0;
                for (i = 2; i < (NCH(n) - 1); i++)
                    l += num_stmts(CHILD(n, i));
                return l;
            }
        default: {
            char buf[128];

            sprintf(buf, "Non-statement found: %d %d\n",
                    TYPE(n), NCH(n));
            Py_FatalError(buf);
        }
    }
    assert(0);
    return 0;
}

/* Transform the CST rooted at node * to the appropriate AST
*/

mod_ty
PyAST_FromNode(const node *n, PyCompilerFlags *flags, const char *filename,
               PyArena *arena)
{
    int i, j, k, num;
    asdl_seq *stmts = NULL;
    stmt_ty s;
    node *ch;
    struct compiling c;

    if (flags && flags->cf_flags & PyCF_SOURCE_IS_UTF8) {
        c.c_encoding = "utf-8";
        if (TYPE(n) == encoding_decl) {
            ast_error(n, "encoding declaration in Unicode string");
            goto error;
        }
    } else if (TYPE(n) == encoding_decl) {
        c.c_encoding = STR(n);
        n = CHILD(n, 0);
    } else {
        c.c_encoding = NULL;
    }
    c.c_future_unicode = flags && flags->cf_flags & CO_FUTURE_UNICODE_LITERALS;
    c.c_no_folding = (flags->cf_flags & PyCF_ONLY_AST) != 0;
	c.c_strict_folding = 0;
    c.c_arena = arena;
    c.c_filename = filename;

    k = 0;
    switch (TYPE(n)) {
        case file_input:
            stmts = asdl_seq_new(num_stmts(n), arena);
            if (!stmts)
                return NULL;
            for (i = 0; i < NCH(n) - 1; i++) {
                ch = CHILD(n, i);
                if (TYPE(ch) == NEWLINE)
                    continue;
                REQ(ch, stmt);
                num = num_stmts(ch);
                if (num == 1) {
                    s = ast_for_stmt(&c, ch);
                    if (!s)
                        goto error;
                    asdl_seq_SET(stmts, k++, s);
                }
                else {
                    ch = CHILD(ch, 0);
                    REQ(ch, simple_stmt);
                    for (j = 0; j < num; j++) {
                        s = ast_for_stmt(&c, CHILD(ch, j * 2));
                        if (!s)
                            goto error;
                        asdl_seq_SET(stmts, k++, s);
                    }
                }
            }
            return Module(stmts, arena);
        case eval_input: {
            expr_ty testlist_ast;

            /* XXX Why not gen_for here? */
            testlist_ast = ast_for_testlist(&c, CHILD(n, 0));
            if (!testlist_ast)
                goto error;
            return Expression(testlist_ast, arena);
        }
        case single_input:
            if (TYPE(CHILD(n, 0)) == NEWLINE) {
                stmts = asdl_seq_new(1, arena);
                if (!stmts)
                    goto error;
                asdl_seq_SET(stmts, 0, Pass(n->n_lineno, n->n_col_offset,
                                            arena));
                if (!asdl_seq_GET(stmts, 0))
                    goto error;
                return Interactive(stmts, arena);
            }
            else {
                n = CHILD(n, 0);
                num = num_stmts(n);
                stmts = asdl_seq_new(num, arena);
                if (!stmts)
                    goto error;
                if (num == 1) {
                    s = ast_for_stmt(&c, n);
                    if (!s)
                        goto error;
                    asdl_seq_SET(stmts, 0, s);
                }
                else {
                    /* Only a simple_stmt can contain multiple statements. */
                    REQ(n, simple_stmt);
                    for (i = 0; i < NCH(n); i += 2) {
                        if (TYPE(CHILD(n, i)) == NEWLINE)
                            break;
                        s = ast_for_stmt(&c, CHILD(n, i));
                        if (!s)
                            goto error;
                        asdl_seq_SET(stmts, i / 2, s);
                    }
                }

                return Interactive(stmts, arena);
            }
        default:
            PyErr_Format(PyExc_SystemError,
                         "invalid node %d for PyAST_FromNode", TYPE(n));
            goto error;
    }
 error:
    ast_error_finish(filename);
    return NULL;
}

/* Return a Python object which is a constant */

static expr_ty
Const(object c, enum _expr_const constant, int lineno, int col_offset, PyArena *arena)
{
        expr_ty p;
        p = (expr_ty)PyArena_Malloc(arena, sizeof(*p));
        if (!p)
                return NULL;
        p->kind = Const_kind;
        p->v.Const.c = c;
		p->v.Const.constant = constant;
        p->lineno = lineno;
        p->col_offset = col_offset;
        return p;
}

/* Return the AST repr. of the operator represented as syntax (|, ^, etc.)
*/

static operator_ty
get_operator(const node *n)
{
    switch (TYPE(n)) {
        case VBAR:
            return BitOr;
        case CIRCUMFLEX:
            return BitXor;
        case AMPER:
            return BitAnd;
        case LEFTSHIFT:
            return LShift;
        case RIGHTSHIFT:
            return RShift;
        case PLUS:
            return Add;
        case MINUS:
            return Sub;
        case STAR:
            return Mult;
        case SLASH:
            return Div;
        case DOUBLESLASH:
            return FloorDiv;
        case PERCENT:
            return Mod;
        default:
            return (operator_ty)0;
    }
}

/* Set the context ctx for expr_ty e, recursively traversing e.

   Only sets context for expr kinds that "can appear in assignment context"
   (according to ../Parser/Python.asdl).  For other expr kinds, it sets
   an appropriate syntax error and returns false.
*/

static int
set_context(struct compiling *c, expr_ty e, expr_context_ty ctx, const node *n)
{
    asdl_seq *s = NULL;
    /* If a particular expression type can't be used for assign / delete,
       set expr_name to its name and an error message will be generated.
    */
    const char* expr_name = NULL;

    /* The ast defines augmented store and load contexts, but the
       implementation here doesn't actually use them.  The code may be
       a little more complex than necessary as a result.  It also means
       that expressions in an augmented assignment have a Store context.
       Consider restructuring so that augmented assignment uses
       set_context(), too.
    */
    assert(ctx != AugStore && ctx != AugLoad);

    switch (e->kind) {
        case Attribute_kind:
            if (ctx == Store && !forbidden_check(c, n,
                                PyBytes_AS_STRING(e->v.Attribute.attr)))
                    return 0;
            e->v.Attribute.ctx = ctx;
            break;
        case Subscript_kind:
            e->v.Subscript.ctx = ctx;
            break;
        case Name_kind:
            if (ctx == Store && !forbidden_check(c, n,
                                PyBytes_AS_STRING(e->v.Name.id)))
                    return 0;
            e->v.Name.ctx = ctx;
            break;
        case List_kind:
            e->v.List.ctx = ctx;
            s = e->v.List.elts;
            break;
        case Tuple_kind:
            if (asdl_seq_LEN(e->v.Tuple.elts) == 0) 
                return ast_error(n, "can't assign to ()");
            e->v.Tuple.ctx = ctx;
            s = e->v.Tuple.elts;
            break;
        case Lambda_kind:
            expr_name = "lambda";
            break;
        case Call_kind:
            expr_name = "function call";
            break;
        case BoolOp_kind:
        case BinOp_kind:
        case UnaryOp_kind:
            expr_name = "operator";
            break;
        case GeneratorExp_kind:
            expr_name = "generator expression";
            break;
        case Yield_kind:
            expr_name = "yield expression";
            break;
        case ListComp_kind:
            expr_name = "list comprehension";
            break;
        case Dict_kind:
        case Num_kind:
        case Str_kind:
        case Const_kind:
            expr_name = "literal";
            break;
        case Compare_kind:
            expr_name = "comparison";
            break;
        case Repr_kind:
            expr_name = "repr";
            break;
        case IfExp_kind:
            expr_name = "conditional expression";
            break;
        default:
            PyErr_Format(PyExc_SystemError, 
                         "unexpected expression in assignment %d (line %d)", 
                         e->kind, e->lineno);
            return 0;
    }
    /* Check for error string set by switch */
    if (expr_name) {
        char buf[300];
        PyOS_snprintf(buf, sizeof(buf),
                      "can't %s %s",
                      ctx == Store ? "assign to" : "delete",
                      expr_name);
        return ast_error(n, buf);
    }

    /* If the LHS is a list or tuple, we need to set the assignment
       context for all the contained elements.  
    */
    if (s) {
        int i;

        for (i = 0; i < asdl_seq_LEN(s); i++) {
            if (!set_context(c, (expr_ty)asdl_seq_GET(s, i), ctx, n))
                return 0;
        }
    }
    return 1;
}

static operator_ty
ast_for_augassign(struct compiling *c, const node *n)
{
    REQ(n, augassign);
    n = CHILD(n, 0);
    switch (STR(n)[0]) {
        case '+':
            return Add;
        case '-':
            return Sub;
        case '/':
            if (STR(n)[1] == '/')
                return FloorDiv;
            else
                return Div;
        case '%':
            return Mod;
        case '<':
            return LShift;
        case '>':
            return RShift;
        case '&':
            return BitAnd;
        case '^':
            return BitXor;
        case '|':
            return BitOr;
        case '*':
            if (STR(n)[1] == '*')
                return Pow;
            else
                return Mult;
        default:
            PyErr_Format(PyExc_SystemError, "invalid augassign: %s", STR(n));
            return (operator_ty)0;
    }
}

static cmpop_ty
ast_for_comp_op(struct compiling *c, const node *n)
{
    /* comp_op: '<'|'>'|'=='|'>='|'<='|'<>'|'!='|'in'|'not' 'in'|'is'
               |'is' 'not'
    */
    REQ(n, comp_op);
    if (NCH(n) == 1) {
        n = CHILD(n, 0);
        switch (TYPE(n)) {
            case LESS:
                return Lt;
            case GREATER:
                return Gt;
            case EQEQUAL:                       /* == */
                return Eq;
            case LESSEQUAL:
                return LtE;
            case GREATEREQUAL:
                return GtE;
            case NOTEQUAL:
                return NotEq;
            case NAME:
                if (strcmp(STR(n), "in") == 0)
                    return In;
                if (strcmp(STR(n), "is") == 0)
                    return Is;
            default:
                PyErr_Format(PyExc_SystemError, "invalid comp_op: %s",
                             STR(n));
                return (cmpop_ty)0;
        }
    }
    else if (NCH(n) == 2) {
        /* handle "not in" and "is not" */
        switch (TYPE(CHILD(n, 0))) {
            case NAME:
                if (strcmp(STR(CHILD(n, 1)), "in") == 0)
                    return NotIn;
                if (strcmp(STR(CHILD(n, 0)), "is") == 0)
                    return IsNot;
            default:
                PyErr_Format(PyExc_SystemError, "invalid comp_op: %s %s",
                             STR(CHILD(n, 0)), STR(CHILD(n, 1)));
                return (cmpop_ty)0;
        }
    }
    PyErr_Format(PyExc_SystemError, "invalid comp_op: has %d children",
                 NCH(n));
    return (cmpop_ty)0;
}

static asdl_seq *
seq_for_testlist(struct compiling *c, const node *n,
				 enum _expr_const *constant)
{
    /* testlist: test (',' test)* [','] */
    asdl_seq *seq;
    expr_ty expression;
    int i;
    assert(TYPE(n) == testlist ||
           TYPE(n) == listmaker ||
           TYPE(n) == testlist_gexp ||
           TYPE(n) == testlist_safe ||
           TYPE(n) == testlist1);

    seq = asdl_seq_new((NCH(n) + 1) / 2, c->c_arena);
    if (!seq)
        return NULL;

	/* Defaults to constant value, but if at least one expression wasn't
	   a constant, reverts to no_const. */
	*constant = pure_const;
    for (i = 0; i < NCH(n); i += 2) {
        assert(TYPE(CHILD(n, i)) == test || TYPE(CHILD(n, i)) == old_test);

        expression = ast_for_expr(c, CHILD(n, i));
        if (!expression)
            return NULL;
		/* Updates the constant flag. */
		*constant &= expression->kind == Const_kind ?
			expression->v.Const.constant : no_const;

        assert(i / 2 < seq->size);
        asdl_seq_SET(seq, i / 2, expression);
    }
    return seq;
}

static expr_ty
compiler_complex_args(struct compiling *c, const node *n)
{
    int i, len = (NCH(n) + 1) / 2;
    expr_ty result;
    asdl_seq *args = asdl_seq_new(len, c->c_arena);
    if (!args)
        return NULL;

    /* fpdef: NAME | '(' fplist ')'
       fplist: fpdef (',' fpdef)* [',']
    */
    REQ(n, fplist);
    for (i = 0; i < len; i++) {
        PyObject *arg_id;
        const node *fpdef_node = CHILD(n, 2*i);
        const node *child;
        expr_ty arg;
set_name:
        /* fpdef_node is either a NAME or an fplist */
        child = CHILD(fpdef_node, 0);
        if (TYPE(child) == NAME) {
            if (!forbidden_check(c, n, STR(child)))
                return NULL;
            arg_id = NEW_IDENTIFIER(child);
            if (!arg_id)
                return NULL;
            arg = Name(arg_id, Store, LINENO(child), child->n_col_offset,
                       c->c_arena);
        }
        else {
            assert(TYPE(fpdef_node) == fpdef);
            /* fpdef_node[0] is not a name, so it must be '(', get CHILD[1] */
            child = CHILD(fpdef_node, 1);
            assert(TYPE(child) == fplist);
            /* NCH == 1 means we have (x), we need to elide the extra parens */
            if (NCH(child) == 1) {
                fpdef_node = CHILD(child, 0);
                assert(TYPE(fpdef_node) == fpdef);
                goto set_name;
            }
            arg = compiler_complex_args(c, child);
        }
        asdl_seq_SET(args, i, arg);
    }

    result = Tuple(args, Store, LINENO(n), n->n_col_offset, c->c_arena);
    if (!set_context(c, result, Store, n))
        return NULL;
    return result;
}


/* Create AST for argument list. */

static arguments_ty
ast_for_arguments(struct compiling *c, const node *n)
{
    /* parameters: '(' [varargslist] ')'
       varargslist: (fpdef ['=' test] ',')* ('*' NAME [',' '**' NAME]
            | '**' NAME) | fpdef ['=' test] (',' fpdef ['=' test])* [',']
    */
    int i, j, k, n_args = 0, n_defaults = 0, found_default = 0;
    asdl_seq *args, *defaults;
    identifier vararg = NULL, kwarg = NULL;
    node *ch;

    if (TYPE(n) == parameters) {
        if (NCH(n) == 2) /* () as argument list */
            return arguments(NULL, NULL, NULL, NULL, c->c_arena);
        n = CHILD(n, 1);
    }
    REQ(n, varargslist);

    /* first count the number of normal args & defaults */
    for (i = 0; i < NCH(n); i++) {
        ch = CHILD(n, i);
        if (TYPE(ch) == fpdef)
            n_args++;
        if (TYPE(ch) == EQUAL)
            n_defaults++;
    }
    args = (n_args ? asdl_seq_new(n_args, c->c_arena) : NULL);
    if (!args && n_args)
        return NULL; /* Don't need to goto error; no objects allocated */
    defaults = (n_defaults ? asdl_seq_new(n_defaults, c->c_arena) : NULL);
    if (!defaults && n_defaults)
        return NULL; /* Don't need to goto error; no objects allocated */

    /* fpdef: NAME | '(' fplist ')'
       fplist: fpdef (',' fpdef)* [',']
    */
    i = 0;
    j = 0;  /* index for defaults */
    k = 0;  /* index for args */
    while (i < NCH(n)) {
        ch = CHILD(n, i);
        switch (TYPE(ch)) {
            case fpdef:
            handle_fpdef:
                /* XXX Need to worry about checking if TYPE(CHILD(n, i+1)) is
                   anything other than EQUAL or a comma? */
                /* XXX Should NCH(n) check be made a separate check? */
                if (i + 1 < NCH(n) && TYPE(CHILD(n, i + 1)) == EQUAL) {
                    expr_ty expression = ast_for_expr(c, CHILD(n, i + 2));
                    if (!expression)
                        goto error;
                    assert(defaults != NULL);
                    asdl_seq_SET(defaults, j++, expression);
                    i += 2;
                    found_default = 1;
                }
                else if (found_default) {
                    ast_error(n, 
                             "non-default argument follows default argument");
                    goto error;
                }
                if (NCH(ch) == 3) {
                    ch = CHILD(ch, 1);
                    /* def foo((x)): is not complex, special case. */
                    if (NCH(ch) != 1) {
                        /* We have complex arguments, setup for unpacking. */
                        if (Py_Py3kWarningFlag && !ast_warn(c, ch,
                            "tuple parameter unpacking has been removed in 3.x"))
                            goto error;
                        asdl_seq_SET(args, k++, compiler_complex_args(c, ch));
                        if (!asdl_seq_GET(args, k-1))
                                goto error;
                    } else {
                        /* def foo((x)): setup for checking NAME below. */
                        /* Loop because there can be many parens and tuple
                           unpacking mixed in. */
                        ch = CHILD(ch, 0);
                        assert(TYPE(ch) == fpdef);
                        goto handle_fpdef;
                    }
                }
                if (TYPE(CHILD(ch, 0)) == NAME) {
                    PyObject *id;
                    expr_ty name;
                    if (!forbidden_check(c, n, STR(CHILD(ch, 0))))
                        goto error;
                    id = NEW_IDENTIFIER(CHILD(ch, 0));
                    if (!id)
                        goto error;
                    name = Name(id, Param, LINENO(ch), ch->n_col_offset,
                                c->c_arena);
                    if (!name)
                        goto error;
                    asdl_seq_SET(args, k++, name);
                                         
                }
                i += 2; /* the name and the comma */
                break;
            case STAR:
                if (!forbidden_check(c, CHILD(n, i+1), STR(CHILD(n, i+1))))
                    goto error;
                vararg = NEW_IDENTIFIER(CHILD(n, i+1));
                if (!vararg)
                    goto error;
                i += 3;
                break;
            case DOUBLESTAR:
                if (!forbidden_check(c, CHILD(n, i+1), STR(CHILD(n, i+1))))
                    goto error;
                kwarg = NEW_IDENTIFIER(CHILD(n, i+1));
                if (!kwarg)
                    goto error;
                i += 3;
                break;
            default:
                PyErr_Format(PyExc_SystemError,
                             "unexpected node in varargslist: %d @ %d",
                             TYPE(ch), i);
                goto error;
        }
    }

    return arguments(args, vararg, kwarg, defaults, c->c_arena);

 error:
    Py_XDECREF(vararg);
    Py_XDECREF(kwarg);
    return NULL;
}

static expr_ty
ast_for_dotted_name(struct compiling *c, const node *n)
{
    expr_ty e;
    identifier id;
    int lineno, col_offset;
    int i;

    REQ(n, dotted_name);

    lineno = LINENO(n);
    col_offset = n->n_col_offset;

    id = NEW_IDENTIFIER(CHILD(n, 0));
    if (!id)
        return NULL;
    e = Name(id, Load, lineno, col_offset, c->c_arena);
    if (!e)
        return NULL;

    for (i = 2; i < NCH(n); i+=2) {
        id = NEW_IDENTIFIER(CHILD(n, i));
        if (!id)
            return NULL;
        e = Attribute(e, id, Load, lineno, col_offset, c->c_arena);
        if (!e)
            return NULL;
    }

    return e;
}

static expr_ty
ast_for_decorator(struct compiling *c, const node *n)
{
    /* decorator: '@' dotted_name [ '(' [arglist] ')' ] NEWLINE */
    expr_ty d = NULL;
    expr_ty name_expr;
    
    REQ(n, decorator);
    REQ(CHILD(n, 0), AT);
    REQ(RCHILD(n, -1), NEWLINE);
    
    name_expr = ast_for_dotted_name(c, CHILD(n, 1));
    if (!name_expr)
        return NULL;
        
    if (NCH(n) == 3) { /* No arguments */
        d = name_expr;
        name_expr = NULL;
    }
    else if (NCH(n) == 5) { /* Call with no arguments */
        d = Call(name_expr, NULL, NULL, NULL, NULL, LINENO(n),
                 n->n_col_offset, c->c_arena);
        if (!d)
            return NULL;
        name_expr = NULL;
    }
    else {
        d = ast_for_call(c, CHILD(n, 3), name_expr);
        if (!d)
            return NULL;
        name_expr = NULL;
    }

    return d;
}

static asdl_seq*
ast_for_decorators(struct compiling *c, const node *n)
{
    asdl_seq* decorator_seq;
    expr_ty d;
    int i;
    
    REQ(n, decorators);
    decorator_seq = asdl_seq_new(NCH(n), c->c_arena);
    if (!decorator_seq)
        return NULL;
        
    for (i = 0; i < NCH(n); i++) {
        d = ast_for_decorator(c, CHILD(n, i));
            if (!d)
                return NULL;
            asdl_seq_SET(decorator_seq, i, d);
    }
    return decorator_seq;
}

static stmt_ty
ast_for_funcdef(struct compiling *c, const node *n, asdl_seq *decorator_seq)
{
    /* funcdef: 'def' NAME parameters ':' suite */
    identifier name;
    arguments_ty args;
    asdl_seq *body;
    int name_i = 1;

    REQ(n, funcdef);

    name = NEW_IDENTIFIER(CHILD(n, name_i));
    if (!name)
        return NULL;
    else if (!forbidden_check(c, CHILD(n, name_i), STR(CHILD(n, name_i))))
        return NULL;
    args = ast_for_arguments(c, CHILD(n, name_i + 1));
    if (!args)
        return NULL;
    body = ast_for_suite(c, CHILD(n, name_i + 3));
    if (!body)
        return NULL;

    return FunctionDef(name, args, body, decorator_seq, LINENO(n),
                       n->n_col_offset, c->c_arena);
}

static stmt_ty
ast_for_decorated(struct compiling *c, const node *n)
{
    /* decorated: decorators (classdef | funcdef) */
    stmt_ty thing = NULL;
    asdl_seq *decorator_seq = NULL;

    REQ(n, decorated);

    decorator_seq = ast_for_decorators(c, CHILD(n, 0));
    if (!decorator_seq)
      return NULL;

    assert(TYPE(CHILD(n, 1)) == funcdef ||
	   TYPE(CHILD(n, 1)) == classdef);

    if (TYPE(CHILD(n, 1)) == funcdef) {
      thing = ast_for_funcdef(c, CHILD(n, 1), decorator_seq);
    } else if (TYPE(CHILD(n, 1)) == classdef) {
      thing = ast_for_classdef(c, CHILD(n, 1), decorator_seq);
    }
    /* we count the decorators in when talking about the class' or
       function's line number */
    if (thing) {
        thing->lineno = LINENO(n);
        thing->col_offset = n->n_col_offset;
    }
    return thing;
}

static expr_ty
ast_for_lambdef(struct compiling *c, const node *n)
{
    /* lambdef: 'lambda' [varargslist] ':' test */
    arguments_ty args;
    expr_ty expression;

    if (NCH(n) == 3) {
        args = arguments(NULL, NULL, NULL, NULL, c->c_arena);
        if (!args)
            return NULL;
        expression = ast_for_expr(c, CHILD(n, 2));
        if (!expression)
            return NULL;
    }
    else {
        args = ast_for_arguments(c, CHILD(n, 1));
        if (!args)
            return NULL;
        expression = ast_for_expr(c, CHILD(n, 3));
        if (!expression)
            return NULL;
    }

    return Lambda(args, expression, LINENO(n), n->n_col_offset, c->c_arena);
}

static expr_ty
ast_for_ifexpr(struct compiling *c, const node *n)
{
    /* test: or_test 'if' or_test 'else' test */ 
    expr_ty expression, body, orelse;

    assert(NCH(n) == 5);
    body = ast_for_expr(c, CHILD(n, 0));
    if (!body)
        return NULL;
    expression = ast_for_expr(c, CHILD(n, 2));
    if (!expression)
        return NULL;
    orelse = ast_for_expr(c, CHILD(n, 4));
    if (!orelse)
        return NULL;
    return IfExp(expression, body, orelse, LINENO(n), n->n_col_offset,
			  c->c_arena);
}

/* XXX(nnorwitz): the listcomp and genexpr code should be refactored
   so there is only a single version.  Possibly for loops can also re-use
   the code.
*/

/* Count the number of 'for' loop in a list comprehension.

   Helper for ast_for_listcomp().
*/

static int
count_list_fors(struct compiling *c, const node *n)
{
    int n_fors = 0;
    node *ch = CHILD(n, 1);

 count_list_for:
    n_fors++;
    REQ(ch, list_for);
    if (NCH(ch) == 5)
        ch = CHILD(ch, 4);
    else
        return n_fors;
 count_list_iter:
    REQ(ch, list_iter);
    ch = CHILD(ch, 0);
    if (TYPE(ch) == list_for)
        goto count_list_for;
    else if (TYPE(ch) == list_if) {
        if (NCH(ch) == 3) {
            ch = CHILD(ch, 2);
            goto count_list_iter;
        }
        else
            return n_fors;
    }

    /* Should never be reached */
    PyErr_SetString(PyExc_SystemError, "logic error in count_list_fors");
    return -1;
}

/* Count the number of 'if' statements in a list comprehension.

   Helper for ast_for_listcomp().
*/

static int
count_list_ifs(struct compiling *c, const node *n)
{
    int n_ifs = 0;

 count_list_iter:
    REQ(n, list_iter);
    if (TYPE(CHILD(n, 0)) == list_for)
        return n_ifs;
    n = CHILD(n, 0);
    REQ(n, list_if);
    n_ifs++;
    if (NCH(n) == 2)
        return n_ifs;
    n = CHILD(n, 2);
    goto count_list_iter;
}

static expr_ty
ast_for_listcomp(struct compiling *c, const node *n)
{
    /* listmaker: test ( list_for | (',' test)* [','] )
       list_for: 'for' exprlist 'in' testlist_safe [list_iter]
       list_iter: list_for | list_if
       list_if: 'if' test [list_iter]
       testlist_safe: test [(',' test)+ [',']]
    */
    expr_ty elt;
    asdl_seq *listcomps;
    int i, n_fors;
    node *ch;

    REQ(n, listmaker);
    assert(NCH(n) > 1);

    elt = ast_for_expr(c, CHILD(n, 0));
    if (!elt)
        return NULL;

    n_fors = count_list_fors(c, n);
    if (n_fors == -1)
        return NULL;

    listcomps = asdl_seq_new(n_fors, c->c_arena);
    if (!listcomps)
        return NULL;

    ch = CHILD(n, 1);
    for (i = 0; i < n_fors; i++) {
        comprehension_ty lc;
        asdl_seq *t;
        expr_ty expression;
        node *for_ch;
        
        REQ(ch, list_for);
        
        for_ch = CHILD(ch, 1);
        t = ast_for_exprlist(c, for_ch, Store);
        if (!t)
            return NULL;
        expression = ast_for_testlist(c, CHILD(ch, 3));
        if (!expression)
            return NULL;
        
        /* Check the # of children rather than the length of t, since
           [x for x, in ... ] has 1 element in t, but still requires a Tuple.
        */
        if (NCH(for_ch) == 1)
            lc = comprehension((expr_ty)asdl_seq_GET(t, 0), expression, NULL,
                               c->c_arena);
        else
            lc = comprehension(Tuple(t, Store, LINENO(ch), ch->n_col_offset,
                                     c->c_arena),
                               expression, NULL, c->c_arena);
        if (!lc)
            return NULL;

        if (NCH(ch) == 5) {
            int j, n_ifs;
            asdl_seq *ifs;
            expr_ty list_for_expr;

            ch = CHILD(ch, 4);
            n_ifs = count_list_ifs(c, ch);
            if (n_ifs == -1)
                return NULL;

            ifs = asdl_seq_new(n_ifs, c->c_arena);
            if (!ifs)
                return NULL;

            for (j = 0; j < n_ifs; j++) {
                REQ(ch, list_iter);
                ch = CHILD(ch, 0);
                REQ(ch, list_if);
                
                list_for_expr = ast_for_expr(c, CHILD(ch, 1));
                if (!list_for_expr)
                    return NULL;
                
                asdl_seq_SET(ifs, j, list_for_expr);
                if (NCH(ch) == 3)
                    ch = CHILD(ch, 2);
            }
            /* on exit, must guarantee that ch is a list_for */
            if (TYPE(ch) == list_iter)
                ch = CHILD(ch, 0);
            lc->ifs = ifs;
        }
        asdl_seq_SET(listcomps, i, lc);
    }

    return ListComp(elt, listcomps, LINENO(n), n->n_col_offset, c->c_arena);
}

/* Count the number of 'for' loops in a generator expression.

   Helper for ast_for_genexp().
*/

static int
count_gen_fors(struct compiling *c, const node *n)
{
    int n_fors = 0;
    node *ch = CHILD(n, 1);

 count_gen_for:
    n_fors++;
    REQ(ch, gen_for);
    if (NCH(ch) == 5)
        ch = CHILD(ch, 4);
    else
        return n_fors;
 count_gen_iter:
    REQ(ch, gen_iter);
    ch = CHILD(ch, 0);
    if (TYPE(ch) == gen_for)
        goto count_gen_for;
    else if (TYPE(ch) == gen_if) {
        if (NCH(ch) == 3) {
            ch = CHILD(ch, 2);
            goto count_gen_iter;
        }
        else
            return n_fors;
    }
    
    /* Should never be reached */
    PyErr_SetString(PyExc_SystemError,
                    "logic error in count_gen_fors");
    return -1;
}

/* Count the number of 'if' statements in a generator expression.

   Helper for ast_for_genexp().
*/

static int
count_gen_ifs(struct compiling *c, const node *n)
{
    int n_ifs = 0;

    while (1) {
        REQ(n, gen_iter);
        if (TYPE(CHILD(n, 0)) == gen_for)
            return n_ifs;
        n = CHILD(n, 0);
        REQ(n, gen_if);
        n_ifs++;
        if (NCH(n) == 2)
            return n_ifs;
        n = CHILD(n, 2);
    }
}

/* TODO(jhylton): Combine with list comprehension code? */
static expr_ty
ast_for_genexp(struct compiling *c, const node *n)
{
    /* testlist_gexp: test ( gen_for | (',' test)* [','] )
       argument: [test '='] test [gen_for]       # Really [keyword '='] test */
    expr_ty elt;
    asdl_seq *genexps;
    int i, n_fors;
    node *ch;
    
    assert(TYPE(n) == (testlist_gexp) || TYPE(n) == (argument));
    assert(NCH(n) > 1);
    
    elt = ast_for_expr(c, CHILD(n, 0));
    if (!elt)
        return NULL;
    
    n_fors = count_gen_fors(c, n);
    if (n_fors == -1)
        return NULL;

    genexps = asdl_seq_new(n_fors, c->c_arena);
    if (!genexps)
        return NULL;

    ch = CHILD(n, 1);
    for (i = 0; i < n_fors; i++) {
        comprehension_ty ge;
        asdl_seq *t;
        expr_ty expression;
        node *for_ch;
        
        REQ(ch, gen_for);
        
        for_ch = CHILD(ch, 1);
        t = ast_for_exprlist(c, for_ch, Store);
        if (!t)
            return NULL;
        expression = ast_for_expr(c, CHILD(ch, 3));
        if (!expression)
            return NULL;

        /* Check the # of children rather than the length of t, since
           (x for x, in ...) has 1 element in t, but still requires a Tuple. */
        if (NCH(for_ch) == 1)
            ge = comprehension((expr_ty)asdl_seq_GET(t, 0), expression,
                               NULL, c->c_arena);
        else
            ge = comprehension(Tuple(t, Store, LINENO(ch), ch->n_col_offset,
                                     c->c_arena),
                               expression, NULL, c->c_arena);

        if (!ge)
            return NULL;

        if (NCH(ch) == 5) {
            int j, n_ifs;
            asdl_seq *ifs;
            
            ch = CHILD(ch, 4);
            n_ifs = count_gen_ifs(c, ch);
            if (n_ifs == -1)
                return NULL;

            ifs = asdl_seq_new(n_ifs, c->c_arena);
            if (!ifs)
                return NULL;

            for (j = 0; j < n_ifs; j++) {
                REQ(ch, gen_iter);
                ch = CHILD(ch, 0);
                REQ(ch, gen_if);
                
                expression = ast_for_expr(c, CHILD(ch, 1));
                if (!expression)
                    return NULL;
                asdl_seq_SET(ifs, j, expression);
                if (NCH(ch) == 3)
                    ch = CHILD(ch, 2);
            }
            /* on exit, must guarantee that ch is a gen_for */
            if (TYPE(ch) == gen_iter)
                ch = CHILD(ch, 0);
            ge->ifs = ifs;
        }
        asdl_seq_SET(genexps, i, ge);
    }
    
    return GeneratorExp(elt, genexps, LINENO(n), n->n_col_offset, c->c_arena);
}

static expr_ty
pyobj_to_expr(PyObject* o, enum _expr_const constant, const node *n, PyArena *arena)
{
	if (constant == no_const)
		if (PyString_CheckExact(o) || PyUnicode_CheckExact(o) ||
			PyBool_Check(o) || PyInt_CheckExact(o) || PyLong_CheckExact(o) ||
			PyFloat_CheckExact(o) || PyComplex_CheckExact(o))
			constant = pure_const;
		else if (PyList_CheckExact(o) || PyDict_CheckExact(o))
			constant = mutable_const;
		else if (PyTuple_CheckExact(o))
			constant = Py_SIZE(o) ? mutable_const : pure_const;
	assert(constant);
	PyArena_AddPyObject(arena, o);
	return Const(o, constant, LINENO(n), n->n_col_offset, arena);
}

static PyObject *
constant_seq_to_pytuple(asdl_seq *elts)
{
	int i;
	PyObject *t, *o, **dest;
	t = PyTuple_New(elts->size);
	if (!t)
		return NULL;
	dest = ((PyTupleObject *) t)->ob_item;
	for(i = 0; i < elts->size; i++) {
		o = ((expr_ty) (elts->elements[i]))->v.Const.c;
		Py_INCREF(o);
		dest[i] = o;
	}
	return t;
}

static PyObject *
constant_seq_to_pylist(asdl_seq *elts)
{
	int i;
	PyObject *t, *o, **dest;
	t = PyList_New(elts->size);
	if (!t)
		return NULL;
	dest = ((PyListObject *) t)->ob_item;
	for(i = 0; i < elts->size; i++) {
		o = ((expr_ty) (elts->elements[i]))->v.Const.c;
		Py_INCREF(o);
		dest[i] = o;
	}
	return t;
}

static PyObject *
constant_seqs_to_pydict(asdl_seq *keys, asdl_seq *values)
{
	int i;
	PyObject *dict = _PyDict_NewPresized(keys->size);
	if (!dict)
		return NULL;
	for(i = 0; i < keys->size; i++) {
		PyObject *k, *v;
		k = ((expr_ty) (keys->elements[i]))->v.Const.c;
		v = ((expr_ty) (values->elements[i]))->v.Const.c;
		if (PyDict_SetItem(dict, k, v) < 0) {
			Py_DECREF(k);
			Py_DECREF(v);
			Py_DECREF(dict);
			return NULL;
		}
	}
	return dict;
}

static expr_ty
ast_for_atom(struct compiling *c, const node *n)
{
    /* atom: '(' [yield_expr|testlist_gexp] ')' | '[' [listmaker] ']'
       | '{' [dictmaker] '}' | '`' testlist '`' | NAME | NUMBER | STRING+
    */
    node *ch = CHILD(n, 0);

    switch (TYPE(ch)) {
    case NAME: {
		PyObject *name;

        /* "None" identifier will be converted to the None constant */
		if (!c->c_no_folding)
			if (!strcmp(STR(ch), "None")) {
				Py_INCREF(Py_None);
				return pyobj_to_expr(Py_None, pure_const, n, c->c_arena);
			}
			/*else if (!strcmp(STR(ch), "False")) {
				Py_INCREF(Py_False);
				return pyobj_to_expr(Py_False, pure_const, n, c->c_arena);
			}
			else if (!strcmp(STR(ch), "True")) {
				Py_INCREF(Py_True);
				return pyobj_to_expr(Py_True, pure_const, n, c->c_arena);
			}*/

        /* All names start in Load context, but may later be
           changed. */
		name = NEW_IDENTIFIER(ch);
		if (!name)
			return NULL;
		return Name(name, Load, LINENO(n), n->n_col_offset, c->c_arena);
	}
    case STRING: {
        PyObject *str = parsestrplus(c, n);
        if (!str) {
#ifdef Py_USING_UNICODE
            if (PyErr_ExceptionMatches(PyExc_UnicodeError)){
                PyObject *type, *value, *tback, *errstr;
                PyErr_Fetch(&type, &value, &tback);
                errstr = PyObject_Str(value);
                if (errstr) {
                    char *s = "";
                    char buf[128];
                    s = PyString_AsString(errstr);
                    PyOS_snprintf(buf, sizeof(buf), "(unicode error) %s", s);
                    ast_error(n, buf);
                    Py_DECREF(errstr);
                } else {
                    ast_error(n, "(unicode error) unknown error");
                }
                Py_DECREF(type);
                Py_DECREF(value);
                Py_XDECREF(tback);
            }
#endif
            return NULL;
        }
        if (c->c_no_folding) {
			PyArena_AddPyObject(c->c_arena, str);
			return Str(str, LINENO(n), n->n_col_offset, c->c_arena);
        }
        else
			return pyobj_to_expr(str, pure_const, n, c->c_arena);
    }
    case NUMBER: {
        PyObject *pynum = parsenumber(c, STR(ch));
		if (!pynum)
			return NULL;
        if (c->c_no_folding) {
			PyArena_AddPyObject(c->c_arena, pynum);
			return Num(pynum, LINENO(n), n->n_col_offset, c->c_arena);
        }
        else
			return pyobj_to_expr(pynum, pure_const, n, c->c_arena);
    }
    case LPAR: /* some parenthesized expressions */
        ch = CHILD(n, 1);
        
        if (TYPE(ch) == RPAR)
	        if (c->c_no_folding)
	            return Tuple(NULL, Load, LINENO(n), n->n_col_offset, c->c_arena);
	        else {
				PyObject *pyemptytuple = PyTuple_New(0);
				if (!pyemptytuple)
					return NULL;
				return pyobj_to_expr(pyemptytuple, pure_const, n, c->c_arena);
			}
		if (TYPE(ch) == yield_expr)
			return ast_for_expr(c, ch);
		if ((NCH(ch) > 1) && (TYPE(CHILD(ch, 1)) == gen_for))
			return ast_for_genexp(c, ch);
		return ast_for_testlist_gexp(c, ch);
    case LSQB: /* list (or list comprehension) */
        ch = CHILD(n, 1);
        
		if (TYPE(ch) == RSQB)
	        if (c->c_no_folding)
	            return List(NULL, Load, LINENO(n), n->n_col_offset, c->c_arena);
	        else {
				PyObject *pyemptylist = PyList_New(0);
				if (!pyemptylist)
					return NULL;
				return pyobj_to_expr(pyemptylist, content_const, n, c->c_arena);
			}
        
        REQ(ch, listmaker);
        if (NCH(ch) == 1 || TYPE(CHILD(ch, 1)) == COMMA) {
			enum _expr_const constant;
            asdl_seq *elts = seq_for_testlist(c, ch, &constant);
            if (!elts)
                return NULL;
			if (!c->c_no_folding && constant) {
				PyObject *np = constant_seq_to_pylist(elts);
				if (np == NULL)
					return NULL;
				return pyobj_to_expr(np, constant == pure_const ?
									 content_const : mutable_const,
									 n, c->c_arena);
			}
			else
				return List(elts, Load, LINENO(n), n->n_col_offset, c->c_arena);
        }
        return ast_for_listcomp(c, ch);
    case LBRACE: {
        /* dictmaker: test ':' test (',' test ':' test)* [','] */
        int i, size;
        asdl_seq *keys, *values;
		enum _expr_const constant;
        
        ch = CHILD(n, 1);
        size = (NCH(ch) + 1) / 4; /* +1 in case no trailing comma */
        keys = asdl_seq_new(size, c->c_arena);
        if (!keys)
            return NULL;
        
        values = asdl_seq_new(size, c->c_arena);
        if (!values)
            return NULL;
        
		/* Defaults to constant value, but if at least one expression wasn't
		   a constant, reverts to no_const. */
		constant = pure_const;
        for (i = 0; i < NCH(ch); i += 4) {
            expr_ty e;
            
            e = ast_for_expr(c, CHILD(ch, i));
            if (!e)
                return NULL;
			if (e->kind == Const_kind) {
				if ((e->v.Const.constant == mutable_const ||
					e->v.Const.constant == content_const) &&
					c->c_strict_folding) {
					char buf[128];
					PyOS_snprintf(buf, sizeof(buf),
								  "unhashable type %s for dictionary key",
								  e->v.Const.c->ob_type->tp_name);
					ast_error(CHILD(ch, i), buf);
					return NULL;
				}
			}
			else
				constant = no_const;

            asdl_seq_SET(keys, i / 4, e);

            e = ast_for_expr(c, CHILD(ch, i + 2));
            if (!e)
                return NULL;
			/* Updates the constant flag. */
			constant &= e->kind == Const_kind ? e->v.Const.constant : no_const;

            asdl_seq_SET(values, i / 4, e);
        }
		if (!c->c_no_folding && constant) {
			PyObject *np = constant_seqs_to_pydict(keys, values);
			if (np == NULL)
				return NULL;
			return pyobj_to_expr(np, constant == pure_const ?
								 content_const : mutable_const, n, c->c_arena);
		}
		return Dict(keys, values, LINENO(n), n->n_col_offset, c->c_arena);
    }
    case BACKQUOTE: { /* repr */
        expr_ty e;
        if (Py_Py3kWarningFlag &&
            !ast_warn(c, n, "backquote not supported in 3.x; use repr()"))
            return NULL;
        e = ast_for_testlist(c, CHILD(n, 1));
        if (!e)
            return NULL;
		if (!c->c_no_folding && e->kind == Const_kind) {
			PyObject *result = PyObject_Repr(e->v.Const.c);
			if (!result)
				if (c->c_strict_folding)
					return ast_check_error(c, n,
										   "invalid backquote operation");
				else {
					PyErr_Clear();
					return Repr(e, LINENO(n), n->n_col_offset, c->c_arena);
				}
			return pyobj_to_expr(result, pure_const, n, c->c_arena);
		}
		return Repr(e, LINENO(n), n->n_col_offset, c->c_arena);
    }
    default:
        PyErr_Format(PyExc_SystemError, "unhandled atom %d", TYPE(ch));
        return NULL;
    }
}

static slice_ty
ast_for_slice(struct compiling *c, const node *n)
{
    node *ch;
    expr_ty lower = NULL, upper = NULL, step = NULL;

    REQ(n, subscript);

    /*
       subscript: '.' '.' '.' | test | [test] ':' [test] [sliceop]
       sliceop: ':' [test]
    */
    ch = CHILD(n, 0);
    if (TYPE(ch) == DOT)
        return Ellipsis(c->c_arena);

    if (NCH(n) == 1 && TYPE(ch) == test) {
        /* 'step' variable hold no significance in terms of being used over
           other vars */
        step = ast_for_expr(c, ch); 
        if (!step)
            return NULL;
            
        return Index(step, c->c_arena);
    }

    if (TYPE(ch) == test) {
        lower = ast_for_expr(c, ch);
        if (!lower)
            return NULL;
    }

    /* If there's an upper bound it's in the second or third position. */
    if (TYPE(ch) == COLON) {
        if (NCH(n) > 1) {
            node *n2 = CHILD(n, 1);

            if (TYPE(n2) == test) {
                upper = ast_for_expr(c, n2);
                if (!upper)
                    return NULL;
            }
        }
    } else if (NCH(n) > 2) {
        node *n2 = CHILD(n, 2);

        if (TYPE(n2) == test) {
            upper = ast_for_expr(c, n2);
            if (!upper)
                return NULL;
        }
    }

    ch = CHILD(n, NCH(n) - 1);
    if (TYPE(ch) == sliceop) {
        if (NCH(ch) == 1) {
            /* No expression, so step is None */
            ch = CHILD(ch, 0);
			if (c->c_no_folding)
				step = Name(new_identifier("None", c->c_arena), Load,
							LINENO(ch), ch->n_col_offset, c->c_arena);
			else {
	            /* None is a constant */
				Py_INCREF(Py_None);
				step = pyobj_to_expr(Py_None, pure_const, ch, c->c_arena);
			}
            if (!step)
                return NULL;
        } else {
            ch = CHILD(ch, 1);
            if (TYPE(ch) == test) {
                step = ast_for_expr(c, ch);
                if (!step)
                    return NULL;
            }
        }
    }

    return Slice(lower, upper, step, c->c_arena);
}

static expr_ty
unfolded_binop(expr_ty expr1, operator_ty newoperator, expr_ty expr2,
		   const node *n, struct compiling *c)
{
	return BinOp(expr1, newoperator, expr2, LINENO(n), n->n_col_offset,
				 c->c_arena);
}

/* Fold binary ops on constants: BINARY_OP(CONST1, CONST2) */
static expr_ty
fold_binop(expr_ty expr1, operator_ty newoperator, expr_ty expr2,
		   const node *n, struct compiling *c)
{
	Py_ssize_t size;
	if (!c->c_no_folding && expr1->kind == Const_kind &&
		expr2->kind == Const_kind) {
		PyObject *obj1, *obj2, *result;
		enum _expr_const constant;
		obj1 = expr1->v.Const.c;
		obj2 = expr2->v.Const.c;
		switch (newoperator) {
			case Add:
				result = PyNumber_Add(obj1, obj2);
				break;
			case Sub:
				result = PyNumber_Subtract(obj1, obj2);
				break;
			case Mult:
				result = PyNumber_Multiply(obj1, obj2);
				break;
			case Div:
				/* Cannot fold this operation statically since
							   the result can depend on the run-time presence
							   of the -Qnew flag */
				return unfolded_binop(expr1, newoperator, expr2, n, c);
			case Mod:
				result = PyNumber_Remainder(obj1, obj2);
				break;
			case Pow:
				result = PyNumber_Power(obj1, obj2, Py_None);
				break;
			case LShift:
				result = PyNumber_Lshift(obj1, obj2);
				break;
			case RShift:
				result = PyNumber_Rshift(obj1, obj2);
				break;
			case BitOr:
				result = PyNumber_Or(obj1, obj2);
				break;
			case BitXor:
				result = PyNumber_Xor(obj1, obj2);
				break;
			case BitAnd:
				result = PyNumber_And(obj1, obj2);
				break;
			case FloorDiv:
				result = PyNumber_FloorDivide(obj1, obj2);
				break;
			default:
                PyErr_Format(PyExc_SystemError, "invalid operator: %d",
                             newoperator);
                return NULL;
		}
		if (!result)
			if (c->c_strict_folding)
				return ast_check_error(c, n, "invalid binary operation");
			else {
				PyErr_Clear();
				return unfolded_binop(expr1, newoperator, expr2, n, c);
			}
		size = PyObject_Size(result);
		if (size == -1)
			PyErr_Clear();
		else if (size > 32) {
			Py_DECREF(result);
			return unfolded_binop(expr1, newoperator, expr2, n, c);
		}
		if (newoperator == Mod && (PyString_CheckExact(obj1) ||
			PyUnicode_CheckExact(obj1)))
			constant = pure_const;
		else
			constant = expr1->v.Const.constant & expr2->v.Const.constant;
		return pyobj_to_expr(result, constant, n, c->c_arena);
	}
	else
		return unfolded_binop(expr1, newoperator, expr2, n, c);
}

static expr_ty
ast_for_binop(struct compiling *c, const node *n)
{
        /* Must account for a sequence of expressions.
           How should A op B op C by represented?  
           BinOp(BinOp(A, op, B), op, C).
        */

        int i, nops;
        expr_ty expr1, expr2;
        operator_ty newoperator;

        expr1 = ast_for_expr(c, CHILD(n, 0));
        if (!expr1)
            return NULL;

        expr2 = ast_for_expr(c, CHILD(n, 2));
        if (!expr2)
            return NULL;

        newoperator = get_operator(CHILD(n, 1));
        if (!newoperator)
            return NULL;

        expr1 = fold_binop(expr1, newoperator, expr2, n, c);
        if (!expr1)
            return NULL;

        nops = (NCH(n) - 1) / 2;
        for (i = 1; i < nops; i++) {
            const node* next_oper = CHILD(n, i * 2 + 1);

            newoperator = get_operator(next_oper);
            if (!newoperator)
                return NULL;

            expr2 = ast_for_expr(c, CHILD(n, i * 2 + 2));
            if (!expr2)
                return NULL;

			expr1 = fold_binop(expr1, newoperator, expr2, next_oper, c);
			if (!expr1)
				return NULL;
        }
        return expr1;
}

static expr_ty
ast_subscript_folding(struct compiling *c, const node *n, expr_ty left_expr,
					  PyObject *right_obj)
{
	PyObject *e;
	e = PyObject_GetItem(left_expr->v.Const.c, right_obj);
	if (!e)
		return ast_check_error(c, n, "invalid subscription operation");
	return pyobj_to_expr(e, no_const, n, c->c_arena);
}

static expr_ty
ast_for_trailer(struct compiling *c, const node *n, expr_ty left_expr)
{
    /* trailer: '(' [arglist] ')' | '[' subscriptlist ']' | '.' NAME 
       subscriptlist: subscript (',' subscript)* [',']
       subscript: '.' '.' '.' | test | [test] ':' [test] [sliceop]
     */
    expr_ty e;
    REQ(n, trailer);
    if (TYPE(CHILD(n, 0)) == LPAR)
        if (NCH(n) == 2)
            return Call(left_expr, NULL, NULL, NULL, NULL, LINENO(n),
						n->n_col_offset, c->c_arena);
        else
            return ast_for_call(c, CHILD(n, 1), left_expr);
    else if (TYPE(CHILD(n, 0)) == DOT ) {
        PyObject *attr_id = NEW_IDENTIFIER(CHILD(n, 1));
        if (!attr_id)
            return NULL;
        return Attribute(left_expr, attr_id, Load,
						 LINENO(n), n->n_col_offset, c->c_arena);
    }
    else {
        REQ(CHILD(n, 0), LSQB);
        REQ(CHILD(n, 2), RSQB);
        n = CHILD(n, 1);
        if (NCH(n) == 1) {
            slice_ty slc = ast_for_slice(c, CHILD(n, 0));
            if (!slc)
                return NULL;
			if (!c->c_no_folding && slc->kind == Index_kind &&
				slc->v.Index.value->kind == Const_kind &&
				left_expr->kind == Const_kind) {
				e = ast_subscript_folding(c, n, left_expr,
												slc->v.Index.value->v.Const.c);
				if (e || c->c_strict_folding)
					return e;
				PyErr_Clear();
			}
            return Subscript(left_expr, slc, Load, LINENO(n), n->n_col_offset,
							 c->c_arena);
        }
        else {
            /* The grammar is ambiguous here. The ambiguity is resolved 
               by treating the sequence as a tuple literal if there are
               no slice features.
            */
            int j;
            slice_ty slc;
            bool simple = true;
			enum _expr_const constant;
            asdl_seq *slices, *elts;
            slices = asdl_seq_new((NCH(n) + 1) / 2, c->c_arena);
            if (!slices)
                return NULL;
			/* Defaults to constant value, but if at least one expression
			   wasn't a constant, reverts to no_const. */
			constant = pure_const;
            for (j = 0; j < NCH(n); j += 2) {
                slc = ast_for_slice(c, CHILD(n, j));
                if (!slc)
                    return NULL;
                if (slc->kind == Index_kind)
					/* Updates the constant flag. */
					constant &= slc->v.Index.value->kind == Const_kind ?
						slc->v.Index.value->v.Const.constant : no_const;
				else
                    simple = false;
                asdl_seq_SET(slices, j / 2, slc);
            }
            if (!simple)
                return Subscript(left_expr, ExtSlice(slices, c->c_arena),
							  Load, LINENO(n), n->n_col_offset, c->c_arena);
			/* extract Index values and put them in a Tuple */
			elts = asdl_seq_new(asdl_seq_LEN(slices), c->c_arena);
			if (!elts)
				return NULL;
			for (j = 0; j < asdl_seq_LEN(slices); ++j) {
				slc = (slice_ty)asdl_seq_GET(slices, j);
				assert(slc->kind == Index_kind  && slc->v.Index.value);
				asdl_seq_SET(elts, j, slc->v.Index.value);
			}
			if (!c->c_no_folding && constant) {
				PyObject *np = constant_seq_to_pytuple(elts);
				if (np == NULL)
					return NULL;
				if (left_expr->kind == Const_kind) {
					e = ast_subscript_folding(c, n, left_expr, np);
					if (e || c->c_strict_folding)
						return e;
					PyErr_Clear();
					e = Tuple(elts, Load, LINENO(n), n->n_col_offset,
							  c->c_arena);
				}
				else
					e = pyobj_to_expr(np, constant == content_const ?
									  mutable_const : constant, n, c->c_arena);
			}
			else
				e = Tuple(elts, Load, LINENO(n), n->n_col_offset, c->c_arena);
			if (!e)
				return NULL;
			return Subscript(left_expr, Index(e, c->c_arena),
						  Load, LINENO(n), n->n_col_offset, c->c_arena);
        }
    }
}

static expr_ty
unfolded_unop(unaryop_ty newoperator, expr_ty expression,
			  const node *n, struct compiling *c)
{
	return UnaryOp(newoperator, expression, LINENO(n), n->n_col_offset,
				   c->c_arena);
}

/* Fold unary ops on constants: UNARY_OP(CONST) */
static expr_ty
fold_unop(unaryop_ty newoperator, expr_ty expression,
		   const node *n, struct compiling *c)
{
	if (!c->c_no_folding && expression->kind == Const_kind) {
		PyObject *obj, *result = NULL;
		obj = expression->v.Const.c;
		switch (newoperator) {
			case Invert:
				result = PyNumber_Invert(obj);
				break;
			case Not: {
				int value = PyObject_Not(obj);
				if (value < 0)
					return ast_check_error(c, n, "invalid not operation");
				result = value ? Py_True : Py_False;
				Py_INCREF(result);
				break;
			}
			case UAdd:
				result = PyNumber_Positive(obj);
				break;
			case USub: {
				int nonzero = PyObject_IsTrue(obj);
				if (nonzero < 0)
					return NULL;
				if (nonzero == 1) {
					result = PyNumber_Negative(obj);
					break;
				}
				else
					/* Preserve the sign of -0.0 */
					return unfolded_unop(USub, expression, n, c);
			}
		}
		if (!result)
			if (c->c_strict_folding)
				return ast_check_error(c, n, "invalid unary operation");
			else {
				PyErr_Clear();
				return unfolded_unop(newoperator, expression, n, c);
			}
		return pyobj_to_expr(result, no_const, n, c->c_arena);
	}
	else
		return unfolded_unop(newoperator, expression, n, c);
}

static expr_ty
ast_for_factor(struct compiling *c, const node *n)
{
    node *pfactor, *ppower, *patom, *pnum;
    expr_ty expression;
	unaryop_ty newoperator;

    /* If the unary - operator is applied to a constant, don't generate
       a UNARY_NEGATIVE opcode.  Just store the approriate value as a
       constant.  The peephole optimizer already does something like
       this but it doesn't handle the case where the constant is
       (sys.maxint - 1).  In that case, we want a PyIntObject, not a
       PyLongObject.
    */
    if (TYPE(CHILD(n, 0)) == MINUS &&
        NCH(n) == 2 &&
        TYPE((pfactor = CHILD(n, 1))) == factor &&
        NCH(pfactor) == 1 &&
        TYPE((ppower = CHILD(pfactor, 0))) == power &&
        NCH(ppower) == 1 &&
        TYPE((patom = CHILD(ppower, 0))) == atom &&
        TYPE((pnum = CHILD(patom, 0))) == NUMBER) {
        char *s = PyObject_MALLOC(strlen(STR(pnum)) + 2);
        if (s == NULL)
            return NULL;
        s[0] = '-';
        strcpy(s + 1, STR(pnum));
        PyObject_FREE(STR(pnum));
        STR(pnum) = s;
        return ast_for_atom(c, patom);
    }

    expression = ast_for_expr(c, CHILD(n, 1));
    if (!expression)
        return NULL;

    switch (TYPE(CHILD(n, 0))) {
        case PLUS:
			newoperator = UAdd;
			break;
        case MINUS:
			newoperator = USub;
			break;
        case TILDE:
			newoperator = Invert;
			break;
		default:
			PyErr_Format(PyExc_SystemError, "unhandled factor: %d",
						 TYPE(CHILD(n, 0)));
			return NULL;
    }
	return fold_unop(newoperator, expression, n, c);
}

static expr_ty
ast_for_power(struct compiling *c, const node *n)
{
    /* power: atom trailer* ('**' factor)*
     */
    int i;
    expr_ty e, tmp;
    REQ(n, power);
    e = ast_for_atom(c, CHILD(n, 0));
    if (!e)
        return NULL;
    if (NCH(n) == 1)
        return e;
    for (i = 1; i < NCH(n); i++) {
        node *ch = CHILD(n, i);
        if (TYPE(ch) != trailer)
            break;
        tmp = ast_for_trailer(c, ch, e);
        if (!tmp)
            return NULL;
        tmp->lineno = e->lineno;
        tmp->col_offset = e->col_offset;
        e = tmp;
    }
    if (TYPE(CHILD(n, NCH(n) - 1)) == factor) {
        expr_ty f = ast_for_expr(c, CHILD(n, NCH(n) - 1));
        if (!f)
            return NULL;
		tmp = fold_binop(e, Pow, f, n, c);
        if (!tmp)
            return NULL;
        e = tmp;
    }
    return e;
}

/* Do not name a variable 'expr'!  Will cause a compile error.
*/

static expr_ty
ast_for_expr(struct compiling *c, const node *n)
{
    /* handle the full range of simple expressions
       test: or_test ['if' or_test 'else' test] | lambdef
       or_test: and_test ('or' and_test)* 
       and_test: not_test ('and' not_test)*
       not_test: 'not' not_test | comparison
       comparison: expr (comp_op expr)*
       expr: xor_expr ('|' xor_expr)*
       xor_expr: and_expr ('^' and_expr)*
       and_expr: shift_expr ('&' shift_expr)*
       shift_expr: arith_expr (('<<'|'>>') arith_expr)*
       arith_expr: term (('+'|'-') term)*
       term: factor (('*'|'/'|'%'|'//') factor)*
       factor: ('+'|'-'|'~') factor | power
       power: atom trailer* ('**' factor)*

       As well as modified versions that exist for backward compatibility,
       to explicitly allow:
       [ x for x in lambda: 0, lambda: 1 ]
       (which would be ambiguous without these extra rules)
       
       old_test: or_test | old_lambdef
       old_lambdef: 'lambda' [vararglist] ':' old_test

    */

    asdl_seq *seq;
    int i;

 loop:
    switch (TYPE(n)) {
        case test:
        case old_test:
            if (TYPE(CHILD(n, 0)) == lambdef ||
                TYPE(CHILD(n, 0)) == old_lambdef)
                return ast_for_lambdef(c, CHILD(n, 0));
            else if (NCH(n) > 1)
                return ast_for_ifexpr(c, n);
            /* Fallthrough */
        case or_test:
        case and_test:
            if (NCH(n) == 1) {
                n = CHILD(n, 0);
                goto loop;
            }
            seq = asdl_seq_new((NCH(n) + 1) / 2, c->c_arena);
            if (!seq)
                return NULL;
            for (i = 0; i < NCH(n); i += 2) {
                expr_ty e = ast_for_expr(c, CHILD(n, i));
                if (!e)
                    return NULL;
                asdl_seq_SET(seq, i / 2, e);
            }
            if (!strcmp(STR(CHILD(n, 1)), "and"))
                return BoolOp(And, seq, LINENO(n), n->n_col_offset,
							  c->c_arena);
			assert(!strcmp(STR(CHILD(n, 1)), "or"));
			return BoolOp(Or, seq, LINENO(n), n->n_col_offset, c->c_arena);
        case not_test:
            if (NCH(n) == 1) {
                n = CHILD(n, 0);
                goto loop;
            }
            else {
                expr_ty expression = ast_for_expr(c, CHILD(n, 1));
                if (!expression)
                    return NULL;
				/* not (a is b) -->  a is not b
				   not (a in b) -->  a not in b
				   not (a is not b) -->  a is b
				   not (a not in b) -->  a in b
				*/
				if (expression->kind == Compare_kind &&
					expression->v.Compare.ops->size == 1 &&
					expression->v.Compare.ops->elements[0] >= Is &&
					expression->v.Compare.ops->elements[0] <= NotIn) {
					expression->v.Compare.ops->elements[0] =
						(expression->v.Compare.ops->elements[0] - 1 ^ 1) + 1;
					return expression;
				}
				else
					return fold_unop(Not, expression, n, c);
            }
        case comparison:
            if (NCH(n) == 1) {
                n = CHILD(n, 0);
                goto loop;
            }
            else {
                expr_ty expression;
                asdl_int_seq *ops;
                asdl_seq *cmps;
                ops = asdl_int_seq_new(NCH(n) / 2, c->c_arena);
                if (!ops)
                    return NULL;
                cmps = asdl_seq_new(NCH(n) / 2, c->c_arena);
                if (!cmps) {
                    return NULL;
                }
                for (i = 1; i < NCH(n); i += 2) {
                    cmpop_ty newoperator;

                    newoperator = ast_for_comp_op(c, CHILD(n, i));
                    if (!newoperator)
                        return NULL;

                    expression = ast_for_expr(c, CHILD(n, i + 1));
                    if (!expression)
                        return NULL;

					if ((expression->kind == Const_kind) &&
						(newoperator == In || newoperator == NotIn)) {
						PyObject *v, *w;
						expression->v.Const.constant = pure_const;
						v = w = expression->v.Const.c;
						if (PyList_CheckExact(v))
							w = PyList_AsTuple(v);
						else if (PyDict_CheckExact(v))
							;
							//w = PyFrozenSet_New(v);
						if (!w)
							return NULL;
						if (v != w) {
							v = w;
							PyArena_AddPyObject(c->c_arena, v);
						}
						expression->v.Const.c = v;
					}
                        
                    asdl_seq_SET(ops, i / 2, newoperator);
                    asdl_seq_SET(cmps, i / 2, expression);
                }
                expression = ast_for_expr(c, CHILD(n, 0));
                if (!expression)
                    return NULL;
                return Compare(expression, ops, cmps, LINENO(n),
							   n->n_col_offset, c->c_arena);
            }
            break;

        /* The next five cases all handle BinOps.  The main body of code
           is the same in each case, but the switch turned inside out to
           reuse the code for each type of operator.
         */
        case expr:
        case xor_expr:
        case and_expr:
        case shift_expr:
        case arith_expr:
        case term:
            if (NCH(n) == 1) {
                n = CHILD(n, 0);
                goto loop;
            }
            return ast_for_binop(c, n);
        case yield_expr: {
            expr_ty exp = NULL;
            if (NCH(n) == 2) {
                exp = ast_for_testlist(c, CHILD(n, 1));
                if (!exp)
                    return NULL;
            }
            return Yield(exp, LINENO(n), n->n_col_offset, c->c_arena);
        }
        case factor:
            if (NCH(n) == 1) {
                n = CHILD(n, 0);
                goto loop;
            }
            return ast_for_factor(c, n);
        case power:
            return ast_for_power(c, n);
        default:
            PyErr_Format(PyExc_SystemError, "unhandled expr: %d", TYPE(n));
            return NULL;
    }
    /* should never get here unless if error is set */
    return NULL;
}

static expr_ty
ast_for_call(struct compiling *c, const node *n, expr_ty func)
{
    /*
      arglist: (argument ',')* (argument [',']| '*' test [',' '**' test]
               | '**' test)
      argument: [test '='] test [gen_for]        # Really [keyword '='] test
    */

    int i, nargs, nkeywords, ngens;
    asdl_seq *args;
    asdl_seq *keywords;
    expr_ty vararg = NULL, kwarg = NULL;

    REQ(n, arglist);

    nargs = 0;
    nkeywords = 0;
    ngens = 0;
    for (i = 0; i < NCH(n); i++) {
        node *ch = CHILD(n, i);
        if (TYPE(ch) == argument) {
            if (NCH(ch) == 1)
                nargs++;
            else if (TYPE(CHILD(ch, 1)) == gen_for)
                ngens++;
            else
                nkeywords++;
        }
    }
    if (ngens > 1 || (ngens && (nargs || nkeywords))) {
        ast_error(n, "Generator expression must be parenthesized "
                  "if not sole argument");
        return NULL;
    }

    if (nargs + nkeywords + ngens > 255) {
      ast_error(n, "more than 255 arguments");
      return NULL;
    }

    args = asdl_seq_new(nargs + ngens, c->c_arena);
    if (!args)
        return NULL;
    keywords = asdl_seq_new(nkeywords, c->c_arena);
    if (!keywords)
        return NULL;
    nargs = 0;
    nkeywords = 0;
    for (i = 0; i < NCH(n); i++) {
        node *ch = CHILD(n, i);
        if (TYPE(ch) == argument) {
            expr_ty e;
            if (NCH(ch) == 1) {
                if (nkeywords) {
                    ast_error(CHILD(ch, 0),
                              "non-keyword arg after keyword arg");
                    return NULL;
                }
                if (vararg) {
                    ast_error(CHILD(ch, 0),
                              "only named arguments may follow *expression");
                    return NULL;
                }
                e = ast_for_expr(c, CHILD(ch, 0));
                if (!e)
                    return NULL;
                asdl_seq_SET(args, nargs++, e);
            }  
            else if (TYPE(CHILD(ch, 1)) == gen_for) {
                e = ast_for_genexp(c, ch);
                if (!e)
                    return NULL;
                asdl_seq_SET(args, nargs++, e);
            }
            else {
                keyword_ty kw;
                identifier key;
                int k;
                char *tmp;

                /* CHILD(ch, 0) is test, but must be an identifier? */ 
                e = ast_for_expr(c, CHILD(ch, 0));
                if (!e)
                    return NULL;
                /* f(lambda x: x[0] = 3) ends up getting parsed with
                 * LHS test = lambda x: x[0], and RHS test = 3.
                 * SF bug 132313 points out that complaining about a keyword
                 * then is very confusing.
                 */
                if (e->kind == Lambda_kind) {
                    ast_error(CHILD(ch, 0),
                              "lambda cannot contain assignment");
                    return NULL;
                } else if (e->kind != Name_kind) {
                    ast_error(CHILD(ch, 0), "keyword can't be an expression");
                    return NULL;
                }
                key = e->v.Name.id;
                if (!forbidden_check(c, CHILD(ch, 0), PyBytes_AS_STRING(key)))
                    return NULL;
                for (k = 0; k < nkeywords; k++) {
                    tmp = PyString_AS_STRING(
                        ((keyword_ty)asdl_seq_GET(keywords, k))->arg);
                    if (!strcmp(tmp, PyString_AS_STRING(key))) {
                        ast_error(CHILD(ch, 0), "keyword argument repeated");
                        return NULL;
                    }
                }
                e = ast_for_expr(c, CHILD(ch, 2));
                if (!e)
                    return NULL;
                kw = keyword(key, e, c->c_arena);
                if (!kw)
                    return NULL;
                asdl_seq_SET(keywords, nkeywords++, kw);
            }
        }
        else if (TYPE(ch) == STAR) {
            vararg = ast_for_expr(c, CHILD(n, i+1));
            if (!vararg)
                return NULL;
            i++;
        }
        else if (TYPE(ch) == DOUBLESTAR) {
            kwarg = ast_for_expr(c, CHILD(n, i+1));
            if (!kwarg)
                return NULL;
            i++;
        }
    }

    return Call(func, args, keywords, vararg, kwarg, func->lineno,
				func->col_offset, c->c_arena);
}

static expr_ty
ast_for_testlist(struct compiling *c, const node* n)
{
    /* testlist_gexp: test (',' test)* [','] */
    /* testlist: test (',' test)* [','] */
    /* testlist_safe: test (',' test)+ [','] */
    /* testlist1: test (',' test)* */
    assert(NCH(n) > 0);
    if (TYPE(n) == testlist_gexp) {
        if (NCH(n) > 1)
            assert(TYPE(CHILD(n, 1)) != gen_for);
    }
    else {
        assert(TYPE(n) == testlist ||
               TYPE(n) == testlist_safe ||
               TYPE(n) == testlist1);
    }
    if (NCH(n) == 1)
        return ast_for_expr(c, CHILD(n, 0));
    else {
		enum _expr_const constant;
        asdl_seq *tmp = seq_for_testlist(c, n, &constant);
        if (!tmp)
            return NULL;
		if (!c->c_no_folding && constant) {
			PyObject *np = constant_seq_to_pytuple(tmp);
			if (np == NULL)
				return NULL;
			return pyobj_to_expr(np, constant == content_const ?
								 constant = mutable_const : constant,
								 n, c->c_arena);
		}
        else
			return Tuple(tmp, Load, LINENO(n), n->n_col_offset, c->c_arena);
    }
}

static expr_ty
ast_for_testlist_gexp(struct compiling *c, const node* n)
{
    /* testlist_gexp: test ( gen_for | (',' test)* [','] ) */
    /* argument: test [ gen_for ] */
    assert(TYPE(n) == testlist_gexp || TYPE(n) == argument);
    if (NCH(n) > 1 && TYPE(CHILD(n, 1)) == gen_for)
        return ast_for_genexp(c, n);
    return ast_for_testlist(c, n);
}

/* like ast_for_testlist() but returns a sequence */
static asdl_seq*
ast_for_class_bases(struct compiling *c, const node* n)
{
    /* testlist: test (',' test)* [','] */
	enum _expr_const constant;
    assert(NCH(n) > 0);
    REQ(n, testlist);
    if (NCH(n) == 1) {
        expr_ty base;
        asdl_seq *bases = asdl_seq_new(1, c->c_arena);
        if (!bases)
            return NULL;
        base = ast_for_expr(c, CHILD(n, 0));
        if (!base)
            return NULL;
        asdl_seq_SET(bases, 0, base);
        return bases;
    }

    return seq_for_testlist(c, n, &constant);
}

static stmt_ty
ast_for_expr_stmt(struct compiling *c, const node *n)
{
    REQ(n, expr_stmt);
    /* expr_stmt: testlist (augassign (yield_expr|testlist) 
                | ('=' (yield_expr|testlist))*)
       testlist: test (',' test)* [',']
       augassign: '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^='
                | '<<=' | '>>=' | '**=' | '//='
       test: ... here starts the operator precendence dance
     */

    if (NCH(n) == 1) {
        expr_ty e = ast_for_testlist(c, CHILD(n, 0));
        if (!e)
            return NULL;

        return Expr(e, LINENO(n), n->n_col_offset, c->c_arena);
    }
    else if (TYPE(CHILD(n, 1)) == augassign) {
        expr_ty expr1, expr2;
        operator_ty newoperator;
        node *ch = CHILD(n, 0);

        expr1 = ast_for_testlist(c, ch);
        if (!expr1)
            return NULL;
        /* TODO(nas): Remove duplicated error checks (set_context does it) */
        switch (expr1->kind) {
            case GeneratorExp_kind:
                ast_error(ch, "augmented assignment to generator "
                          "expression not possible");
                return NULL;
            case Yield_kind:
                ast_error(ch, "augmented assignment to yield "
                          "expression not possible");
                return NULL;
            case Name_kind: {
                const char *var_name = PyBytes_AS_STRING(expr1->v.Name.id);
                if ((var_name[0] == 'N' || var_name[0] == 'T' || var_name[0] == 'F') &&
                    !forbidden_check(c, ch, var_name))
                    return NULL;
                break;
            }
            case Attribute_kind:
            case Subscript_kind:
                break;
            default:
                ast_error(ch, "illegal expression for augmented "
                          "assignment");
                return NULL;
        }
        if(!set_context(c, expr1, Store, ch))
            return NULL;

        ch = CHILD(n, 2);
        if (TYPE(ch) == testlist)
            expr2 = ast_for_testlist(c, ch);
        else
            expr2 = ast_for_expr(c, ch);
        if (!expr2)
            return NULL;

        newoperator = ast_for_augassign(c, CHILD(n, 1));
        if (!newoperator)
            return NULL;

        return AugAssign(expr1, newoperator, expr2, LINENO(n), n->n_col_offset,
                         c->c_arena);
    }
    else {
        int i;
        asdl_seq *targets;
        node *value;
        expr_ty expression;

        /* a normal assignment */
        REQ(CHILD(n, 1), EQUAL);
        targets = asdl_seq_new(NCH(n) / 2, c->c_arena);
        if (!targets)
            return NULL;
        for (i = 0; i < NCH(n) - 2; i += 2) {
            expr_ty e;
            node *ch = CHILD(n, i);
            if (TYPE(ch) == yield_expr) {
                ast_error(ch, "assignment to yield expression not possible");
                return NULL;
            }
            e = ast_for_testlist(c, ch);

            /* set context to assign */
            if (!e) 
                return NULL;

            if (!set_context(c, e, Store, CHILD(n, i)))
                return NULL;

            asdl_seq_SET(targets, i / 2, e);
        }
        value = CHILD(n, NCH(n) - 1);
        if (TYPE(value) == testlist)
            expression = ast_for_testlist(c, value);
        else
            expression = ast_for_expr(c, value);
        if (!expression)
            return NULL;
        return Assign(targets, expression, LINENO(n), n->n_col_offset,
                      c->c_arena);
    }
}

static stmt_ty
ast_for_print_stmt(struct compiling *c, const node *n)
{
    /* print_stmt: 'print' ( [ test (',' test)* [','] ]
                             | '>>' test [ (',' test)+ [','] ] )
     */
    expr_ty dest = NULL, expression;
    asdl_seq *seq;
    bool nl;
    int i, j, start = 1;

    REQ(n, print_stmt);
    if (NCH(n) >= 2 && TYPE(CHILD(n, 1)) == RIGHTSHIFT) {
        dest = ast_for_expr(c, CHILD(n, 2));
        if (!dest)
            return NULL;
            start = 4;
    }
    seq = asdl_seq_new((NCH(n) + 1 - start) / 2, c->c_arena);
    if (!seq)
        return NULL;
    for (i = start, j = 0; i < NCH(n); i += 2, ++j) {
        expression = ast_for_expr(c, CHILD(n, i));
        if (!expression)
            return NULL;
        asdl_seq_SET(seq, j, expression);
    }
    nl = (TYPE(CHILD(n, NCH(n) - 1)) == COMMA) ? false : true;
    return Print(dest, seq, nl, LINENO(n), n->n_col_offset, c->c_arena);
}

static asdl_seq *
ast_for_exprlist(struct compiling *c, const node *n, expr_context_ty context)
{
    asdl_seq *seq;
    int i;
    expr_ty e;

    REQ(n, exprlist);

    seq = asdl_seq_new((NCH(n) + 1) / 2, c->c_arena);
    if (!seq)
        return NULL;
    for (i = 0; i < NCH(n); i += 2) {
        e = ast_for_expr(c, CHILD(n, i));
        if (!e)
            return NULL;
        asdl_seq_SET(seq, i / 2, e);
        if (context && !set_context(c, e, context, CHILD(n, i)))
            return NULL;
    }
    return seq;
}

static stmt_ty
ast_for_del_stmt(struct compiling *c, const node *n)
{
    asdl_seq *expr_list;
    
    /* del_stmt: 'del' exprlist */
    REQ(n, del_stmt);

    expr_list = ast_for_exprlist(c, CHILD(n, 1), Del);
    if (!expr_list)
        return NULL;
    return Delete(expr_list, LINENO(n), n->n_col_offset, c->c_arena);
}

static stmt_ty
ast_for_flow_stmt(struct compiling *c, const node *n)
{
    /*
      flow_stmt: break_stmt | continue_stmt | return_stmt | raise_stmt
                 | yield_stmt
      break_stmt: 'break'
      continue_stmt: 'continue'
      return_stmt: 'return' [testlist]
      yield_stmt: yield_expr
      yield_expr: 'yield' testlist
      raise_stmt: 'raise' [test [',' test [',' test]]]
    */
    node *ch;

    REQ(n, flow_stmt);
    ch = CHILD(n, 0);
    switch (TYPE(ch)) {
        case break_stmt:
            return Break(LINENO(n), n->n_col_offset, c->c_arena);
        case continue_stmt:
            return Continue(LINENO(n), n->n_col_offset, c->c_arena);
        case yield_stmt: { /* will reduce to yield_expr */
            expr_ty exp = ast_for_expr(c, CHILD(ch, 0));
            if (!exp)
                return NULL;
            return Expr(exp, LINENO(n), n->n_col_offset, c->c_arena);
        }
        case return_stmt:
            if (NCH(ch) == 1)
                return Return(NULL, LINENO(n), n->n_col_offset, c->c_arena);
            else {
                expr_ty expression = ast_for_testlist(c, CHILD(ch, 1));
                if (!expression)
                    return NULL;
                return Return(expression, LINENO(n), n->n_col_offset,
                              c->c_arena);
            }
        case raise_stmt:
            if (NCH(ch) == 1)
                return Raise(NULL, NULL, NULL, LINENO(n), n->n_col_offset,
                             c->c_arena);
            else if (NCH(ch) == 2) {
                expr_ty expression = ast_for_expr(c, CHILD(ch, 1));
                if (!expression)
                    return NULL;
                return Raise(expression, NULL, NULL, LINENO(n),
                             n->n_col_offset, c->c_arena);
            }
            else if (NCH(ch) == 4) {
                expr_ty expr1, expr2;

                expr1 = ast_for_expr(c, CHILD(ch, 1));
                if (!expr1)
                    return NULL;
                expr2 = ast_for_expr(c, CHILD(ch, 3));
                if (!expr2)
                    return NULL;

                return Raise(expr1, expr2, NULL, LINENO(n), n->n_col_offset,
                             c->c_arena);
            }
            else if (NCH(ch) == 6) {
                expr_ty expr1, expr2, expr3;

                expr1 = ast_for_expr(c, CHILD(ch, 1));
                if (!expr1)
                    return NULL;
                expr2 = ast_for_expr(c, CHILD(ch, 3));
                if (!expr2)
                    return NULL;
                expr3 = ast_for_expr(c, CHILD(ch, 5));
                if (!expr3)
                    return NULL;
                    
                return Raise(expr1, expr2, expr3, LINENO(n), n->n_col_offset,
                             c->c_arena);
            }
        default:
            PyErr_Format(PyExc_SystemError,
                         "unexpected flow_stmt: %d", TYPE(ch));
            return NULL;
    }

    PyErr_SetString(PyExc_SystemError, "unhandled flow statement");
    return NULL;
}

static alias_ty
alias_for_import_name(struct compiling *c, const node *n)
{
    /*
      import_as_name: NAME ['as' NAME]
      dotted_as_name: dotted_name ['as' NAME]
      dotted_name: NAME ('.' NAME)*
    */
    PyObject *str, *name;

 loop:
    switch (TYPE(n)) {
        case import_as_name:
            str = NULL;
            if (NCH(n) == 3) {
                str = NEW_IDENTIFIER(CHILD(n, 2));
                if (!str)
                    return NULL;
            }
            name = NEW_IDENTIFIER(CHILD(n, 0));
            if (!name)
                return NULL;
            return alias(name, str, c->c_arena);
        case dotted_as_name:
            if (NCH(n) == 1) {
                n = CHILD(n, 0);
                goto loop;
            }
            else {
                alias_ty a = alias_for_import_name(c, CHILD(n, 0));
                if (!a)
                    return NULL;
                assert(!a->asname);
                a->asname = NEW_IDENTIFIER(CHILD(n, 2));
                if (!a->asname)
                    return NULL;
                return a;
            }
            break;
        case dotted_name:
            if (NCH(n) == 1) {
                name = NEW_IDENTIFIER(CHILD(n, 0));
                if (!name)
                    return NULL;
                return alias(name, NULL, c->c_arena);
            }
            else {
                /* Create a string of the form "a.b.c" */
                int i;
                size_t len;
                char *s;

                len = 0;
                for (i = 0; i < NCH(n); i += 2)
                    /* length of string plus one for the dot */
                    len += strlen(STR(CHILD(n, i))) + 1;
                len--; /* the last name doesn't have a dot */
                str = PyString_FromStringAndSize(NULL, len);
                if (!str)
                    return NULL;
                s = PyString_AS_STRING(str);
                if (!s)
                    return NULL;
                for (i = 0; i < NCH(n); i += 2) {
                    char *sch = STR(CHILD(n, i));
                    strcpy(s, STR(CHILD(n, i)));
                    s += strlen(sch);
                    *s++ = '.';
                }
                --s;
                *s = '\0';
                PyString_InternInPlace(&str);
                PyArena_AddPyObject(c->c_arena, str);
                return alias(str, NULL, c->c_arena);
            }
            break;
        case STAR:
            str = PyString_InternFromString("*");
            PyArena_AddPyObject(c->c_arena, str);
            return alias(str, NULL, c->c_arena);
        default:
            PyErr_Format(PyExc_SystemError,
                         "unexpected import name: %d", TYPE(n));
            return NULL;
    }

    PyErr_SetString(PyExc_SystemError, "unhandled import name condition");
    return NULL;
}

static stmt_ty
ast_for_import_stmt(struct compiling *c, const node *n)
{
    /*
      import_stmt: import_name | import_from
      import_name: 'import' dotted_as_names
      import_from: 'from' ('.'* dotted_name | '.') 'import'
                          ('*' | '(' import_as_names ')' | import_as_names)
    */
    int lineno;
    int col_offset;
    int i;
    asdl_seq *aliases;

    REQ(n, import_stmt);
    lineno = LINENO(n);
    col_offset = n->n_col_offset;
    n = CHILD(n, 0);
    if (TYPE(n) == import_name) {
        n = CHILD(n, 1);
        REQ(n, dotted_as_names);
        aliases = asdl_seq_new((NCH(n) + 1) / 2, c->c_arena);
        if (!aliases)
            return NULL;
        for (i = 0; i < NCH(n); i += 2) {
            alias_ty import_alias = alias_for_import_name(c, CHILD(n, i));
            if (!import_alias)
                return NULL;
            asdl_seq_SET(aliases, i / 2, import_alias);
        }
        return Import(aliases, lineno, col_offset, c->c_arena);
    }
    else if (TYPE(n) == import_from) {
        int n_children;
        int idx, ndots = 0;
        alias_ty mod = NULL;
        identifier modname;
        
       /* Count the number of dots (for relative imports) and check for the
          optional module name */
        for (idx = 1; idx < NCH(n); idx++) {
            if (TYPE(CHILD(n, idx)) == dotted_name) {
                mod = alias_for_import_name(c, CHILD(n, idx));
                idx++;
                break;
            } else if (TYPE(CHILD(n, idx)) != DOT) {
                break;
            }
            ndots++;
        }
        idx++; /* skip over the 'import' keyword */
        switch (TYPE(CHILD(n, idx))) {
        case STAR:
            /* from ... import * */
            n = CHILD(n, idx);
            n_children = 1;
            break;
        case LPAR:
            /* from ... import (x, y, z) */
            n = CHILD(n, idx + 1);
            n_children = NCH(n);
            break;
        case import_as_names:
            /* from ... import x, y, z */
            n = CHILD(n, idx);
            n_children = NCH(n);
            if (n_children % 2 == 0) {
                ast_error(n, "trailing comma not allowed without"
                             " surrounding parentheses");
                return NULL;
            }
            break;
        default:
            ast_error(n, "Unexpected node-type in from-import");
            return NULL;
        }

        aliases = asdl_seq_new((n_children + 1) / 2, c->c_arena);
        if (!aliases)
            return NULL;

        /* handle "from ... import *" special b/c there's no children */
        if (TYPE(n) == STAR) {
            alias_ty import_alias = alias_for_import_name(c, n);
            if (!import_alias)
                return NULL;
                asdl_seq_SET(aliases, 0, import_alias);
        }
        else {
            for (i = 0; i < NCH(n); i += 2) {
                alias_ty import_alias = alias_for_import_name(c, CHILD(n, i));
                if (!import_alias)
                    return NULL;
                    asdl_seq_SET(aliases, i / 2, import_alias);
            }
        }
        if (mod != NULL)
            modname = mod->name;
        else
            modname = new_identifier("", c->c_arena);
        return ImportFrom(modname, aliases, ndots, lineno, col_offset,
                          c->c_arena);
    }
    PyErr_Format(PyExc_SystemError,
                 "unknown import statement: starts with command '%s'",
                 STR(CHILD(n, 0)));
    return NULL;
}

static stmt_ty
ast_for_global_stmt(struct compiling *c, const node *n)
{
    /* global_stmt: 'global' NAME (',' NAME)* */
    identifier name;
    asdl_seq *s;
    int i;

    REQ(n, global_stmt);
    s = asdl_seq_new(NCH(n) / 2, c->c_arena);
    if (!s)
        return NULL;
    for (i = 1; i < NCH(n); i += 2) {
        name = NEW_IDENTIFIER(CHILD(n, i));
        if (!name)
            return NULL;
        asdl_seq_SET(s, i / 2, name);
    }
    return Global(s, LINENO(n), n->n_col_offset, c->c_arena);
}

static stmt_ty
ast_for_exec_stmt(struct compiling *c, const node *n)
{
    expr_ty expr1, globals = NULL, locals = NULL;
    int n_children = NCH(n);
    if (n_children != 2 && n_children != 4 && n_children != 6) {
        PyErr_Format(PyExc_SystemError,
                     "poorly formed 'exec' statement: %d parts to statement",
                     n_children);
        return NULL;
    }

    /* exec_stmt: 'exec' expr ['in' test [',' test]] */
    REQ(n, exec_stmt);
    expr1 = ast_for_expr(c, CHILD(n, 1));
    if (!expr1)
        return NULL;
    if (n_children >= 4) {
        globals = ast_for_expr(c, CHILD(n, 3));
        if (!globals)
            return NULL;
    }
    if (n_children == 6) {
        locals = ast_for_expr(c, CHILD(n, 5));
        if (!locals)
            return NULL;
    }

    return Exec(expr1, globals, locals, LINENO(n), n->n_col_offset,
                c->c_arena);
}

static stmt_ty
ast_for_assert_stmt(struct compiling *c, const node *n)
{
    /* assert_stmt: 'assert' test [',' test] */
    REQ(n, assert_stmt);
    if (NCH(n) == 2) {
        expr_ty expression = ast_for_expr(c, CHILD(n, 1));
        if (!expression)
            return NULL;
        return Assert(expression, NULL, LINENO(n), n->n_col_offset,
                      c->c_arena);
    }
    else if (NCH(n) == 4) {
        expr_ty expr1, expr2;

        expr1 = ast_for_expr(c, CHILD(n, 1));
        if (!expr1)
            return NULL;
        expr2 = ast_for_expr(c, CHILD(n, 3));
        if (!expr2)
            return NULL;
            
        return Assert(expr1, expr2, LINENO(n), n->n_col_offset, c->c_arena);
    }
    PyErr_Format(PyExc_SystemError,
                 "improper number of parts to 'assert' statement: %d",
                 NCH(n));
    return NULL;
}

static asdl_seq *
ast_for_suite(struct compiling *c, const node *n)
{
    /* suite: simple_stmt | NEWLINE INDENT stmt+ DEDENT */
    asdl_seq *seq;
    stmt_ty s;
    int i, total, num, end, pos = 0;
    node *ch;

    REQ(n, suite);

    total = num_stmts(n);
    seq = asdl_seq_new(total, c->c_arena);
    if (!seq)
        return NULL;
    if (TYPE(CHILD(n, 0)) == simple_stmt) {
        n = CHILD(n, 0);
        /* simple_stmt always ends with a NEWLINE,
           and may have a trailing SEMI 
        */
        end = NCH(n) - 1;
        if (TYPE(CHILD(n, end - 1)) == SEMI)
            end--;
        /* loop by 2 to skip semi-colons */
        for (i = 0; i < end; i += 2) {
            ch = CHILD(n, i);
            s = ast_for_stmt(c, ch);
            if (!s)
                return NULL;
            asdl_seq_SET(seq, pos++, s);
        }
    }
    else {
        for (i = 2; i < (NCH(n) - 1); i++) {
            ch = CHILD(n, i);
            REQ(ch, stmt);
            num = num_stmts(ch);
            if (num == 1) {
                /* small_stmt or compound_stmt with only one child */
                s = ast_for_stmt(c, ch);
                if (!s)
                    return NULL;
                asdl_seq_SET(seq, pos++, s);
            }
            else {
                int j;
                ch = CHILD(ch, 0);
                REQ(ch, simple_stmt);
                for (j = 0; j < NCH(ch); j += 2) {
                    /* statement terminates with a semi-colon ';' */
                    if (NCH(CHILD(ch, j)) == 0) {
                        assert((j + 1) == NCH(ch));
                        break;
                    }
                    s = ast_for_stmt(c, CHILD(ch, j));
                    if (!s)
                        return NULL;
                    asdl_seq_SET(seq, pos++, s);
                }
            }
        }
    }
    assert(pos == seq->size);
    return seq;
}

static stmt_ty
ast_for_if_stmt(struct compiling *c, const node *n)
{
    /* if_stmt: 'if' test ':' suite ('elif' test ':' suite)*
       ['else' ':' suite]
    */
    char *s;

    REQ(n, if_stmt);

    if (NCH(n) == 4) {
        expr_ty expression;
        asdl_seq *suite_seq;

        expression = ast_for_expr(c, CHILD(n, 1));
        if (!expression)
            return NULL;
        suite_seq = ast_for_suite(c, CHILD(n, 3)); 
        if (!suite_seq)
            return NULL;
            
        return If(expression, suite_seq, NULL, LINENO(n), n->n_col_offset,
                  c->c_arena);
    }

    s = STR(CHILD(n, 4));
    /* s[2], the third character in the string, will be
       's' for el_s_e, or
       'i' for el_i_f
    */
    if (s[2] == 's') {
        expr_ty expression;
        asdl_seq *seq1, *seq2;

        expression = ast_for_expr(c, CHILD(n, 1));
        if (!expression)
            return NULL;
        seq1 = ast_for_suite(c, CHILD(n, 3));
        if (!seq1)
            return NULL;
        seq2 = ast_for_suite(c, CHILD(n, 6));
        if (!seq2)
            return NULL;

        return If(expression, seq1, seq2, LINENO(n), n->n_col_offset,
                  c->c_arena);
    }
    else if (s[2] == 'i') {
        int i, n_elif, has_else = 0;
        expr_ty expression;
        asdl_seq *suite_seq;
        asdl_seq *orelse = NULL;
        n_elif = NCH(n) - 4;
        /* must reference the child n_elif+1 since 'else' token is third,
           not fourth, child from the end. */
        if (TYPE(CHILD(n, (n_elif + 1))) == NAME
            && STR(CHILD(n, (n_elif + 1)))[2] == 's') {
            has_else = 1;
            n_elif -= 3;
        }
        n_elif /= 4;

        if (has_else) {
            asdl_seq *suite_seq2;

            orelse = asdl_seq_new(1, c->c_arena);
            if (!orelse)
                return NULL;
            expression = ast_for_expr(c, CHILD(n, NCH(n) - 6));
            if (!expression)
                return NULL;
            suite_seq = ast_for_suite(c, CHILD(n, NCH(n) - 4));
            if (!suite_seq)
                return NULL;
            suite_seq2 = ast_for_suite(c, CHILD(n, NCH(n) - 1));
            if (!suite_seq2)
                return NULL;

            asdl_seq_SET(orelse, 0, 
                         If(expression, suite_seq, suite_seq2, 
                            LINENO(CHILD(n, NCH(n) - 6)),
                            CHILD(n, NCH(n) - 6)->n_col_offset,
                            c->c_arena));
            /* the just-created orelse handled the last elif */
            n_elif--;
        }

        for (i = 0; i < n_elif; i++) {
            int off = 5 + (n_elif - i - 1) * 4;
            asdl_seq *newobj = asdl_seq_new(1, c->c_arena);
            if (!newobj)
                return NULL;
            expression = ast_for_expr(c, CHILD(n, off));
            if (!expression)
                return NULL;
            suite_seq = ast_for_suite(c, CHILD(n, off + 2));
            if (!suite_seq)
                return NULL;

            asdl_seq_SET(newobj, 0,
                         If(expression, suite_seq, orelse, 
                            LINENO(CHILD(n, off)),
                            CHILD(n, off)->n_col_offset, c->c_arena));
            orelse = newobj;
        }
        expression = ast_for_expr(c, CHILD(n, 1));
        if (!expression)
            return NULL;
        suite_seq = ast_for_suite(c, CHILD(n, 3));
        if (!suite_seq)
            return NULL;
        return If(expression, suite_seq, orelse,
                  LINENO(n), n->n_col_offset, c->c_arena);
    }

    PyErr_Format(PyExc_SystemError,
                 "unexpected token in 'if' statement: %s", s);
    return NULL;
}

static stmt_ty
ast_for_while_stmt(struct compiling *c, const node *n)
{
    /* while_stmt: 'while' test ':' suite ['else' ':' suite] */
    REQ(n, while_stmt);

    if (NCH(n) == 4) {
        expr_ty expression;
        asdl_seq *suite_seq;

        expression = ast_for_expr(c, CHILD(n, 1));
        if (!expression)
            return NULL;
        suite_seq = ast_for_suite(c, CHILD(n, 3));
        if (!suite_seq)
            return NULL;
        return While(expression, suite_seq, NULL, LINENO(n), n->n_col_offset,
                     c->c_arena);
    }
    else if (NCH(n) == 7) {
        expr_ty expression;
        asdl_seq *seq1, *seq2;

        expression = ast_for_expr(c, CHILD(n, 1));
        if (!expression)
            return NULL;
        seq1 = ast_for_suite(c, CHILD(n, 3));
        if (!seq1)
            return NULL;
        seq2 = ast_for_suite(c, CHILD(n, 6));
        if (!seq2)
            return NULL;

        return While(expression, seq1, seq2, LINENO(n), n->n_col_offset,
                     c->c_arena);
    }

    PyErr_Format(PyExc_SystemError,
                 "wrong number of tokens for 'while' statement: %d",
                 NCH(n));
    return NULL;
}

static stmt_ty
ast_for_for_stmt(struct compiling *c, const node *n)
{
    asdl_seq *_target, *seq = NULL, *suite_seq;
    expr_ty expression;
    expr_ty target;
    const node *node_target;
    /* for_stmt: 'for' exprlist 'in' testlist ':' suite ['else' ':' suite] */
    REQ(n, for_stmt);

    if (NCH(n) == 9) {
        seq = ast_for_suite(c, CHILD(n, 8));
        if (!seq)
            return NULL;
    }

    node_target = CHILD(n, 1);
    _target = ast_for_exprlist(c, node_target, Store);
    if (!_target)
        return NULL;
    /* Check the # of children rather than the length of _target, since
       for x, in ... has 1 element in _target, but still requires a Tuple. */
    if (NCH(node_target) == 1)
        target = (expr_ty)asdl_seq_GET(_target, 0);
    else
        target = Tuple(_target, Store, LINENO(n), n->n_col_offset, c->c_arena);

    expression = ast_for_testlist(c, CHILD(n, 3));
    if (!expression)
        return NULL;
    suite_seq = ast_for_suite(c, CHILD(n, 5));
    if (!suite_seq)
        return NULL;

    return For(target, expression, suite_seq, seq, LINENO(n), n->n_col_offset,
               c->c_arena);
}

static excepthandler_ty
ast_for_except_clause(struct compiling *c, const node *exc, node *body)
{
    /* except_clause: 'except' [test [(',' | 'as') test]] */
    REQ(exc, except_clause);
    REQ(body, suite);

    if (NCH(exc) == 1) {
        asdl_seq *suite_seq = ast_for_suite(c, body);
        if (!suite_seq)
            return NULL;

        return ExceptHandler(NULL, NULL, suite_seq, LINENO(exc),
                             exc->n_col_offset, c->c_arena);
    }
    else if (NCH(exc) == 2) {
        expr_ty expression;
        asdl_seq *suite_seq;

        expression = ast_for_expr(c, CHILD(exc, 1));
        if (!expression)
            return NULL;
        suite_seq = ast_for_suite(c, body);
        if (!suite_seq)
            return NULL;

        return ExceptHandler(expression, NULL, suite_seq, LINENO(exc),
                             exc->n_col_offset, c->c_arena);
    }
    else if (NCH(exc) == 4) {
        asdl_seq *suite_seq;
        expr_ty expression;
        expr_ty e = ast_for_expr(c, CHILD(exc, 3));
        if (!e)
            return NULL;
        if (!set_context(c, e, Store, CHILD(exc, 3)))
            return NULL;
        expression = ast_for_expr(c, CHILD(exc, 1));
        if (!expression)
            return NULL;
        suite_seq = ast_for_suite(c, body);
        if (!suite_seq)
            return NULL;

        return ExceptHandler(expression, e, suite_seq, LINENO(exc),
                             exc->n_col_offset, c->c_arena);
    }

    PyErr_Format(PyExc_SystemError,
                 "wrong number of children for 'except' clause: %d",
                 NCH(exc));
    return NULL;
}

static stmt_ty
ast_for_try_stmt(struct compiling *c, const node *n)
{
    const int nch = NCH(n);
    int n_except = (nch - 3)/3;
    asdl_seq *body, *orelse = NULL, *finally = NULL;

    REQ(n, try_stmt);

    body = ast_for_suite(c, CHILD(n, 2));
    if (body == NULL)
        return NULL;

    if (TYPE(CHILD(n, nch - 3)) == NAME) {
        if (strcmp(STR(CHILD(n, nch - 3)), "finally") == 0) {
            if (nch >= 9 && TYPE(CHILD(n, nch - 6)) == NAME) {
                /* we can assume it's an "else",
                   because nch >= 9 for try-else-finally and
                   it would otherwise have a type of except_clause */
                orelse = ast_for_suite(c, CHILD(n, nch - 4));
                if (orelse == NULL)
                    return NULL;
                n_except--;
            }

            finally = ast_for_suite(c, CHILD(n, nch - 1));
            if (finally == NULL)
                return NULL;
            n_except--;
        }
        else {
            /* we can assume it's an "else",
               otherwise it would have a type of except_clause */
            orelse = ast_for_suite(c, CHILD(n, nch - 1));
            if (orelse == NULL)
                return NULL;
            n_except--;
        }
    }
    else if (TYPE(CHILD(n, nch - 3)) != except_clause) {
        ast_error(n, "malformed 'try' statement");
        return NULL;
    }
    
    if (n_except > 0) {
        int i;
        stmt_ty except_st;
        /* process except statements to create a try ... except */
        asdl_seq *handlers = asdl_seq_new(n_except, c->c_arena);
        if (handlers == NULL)
            return NULL;

        for (i = 0; i < n_except; i++) {
            excepthandler_ty e = ast_for_except_clause(c, CHILD(n, 3 + i * 3),
                                                       CHILD(n, 5 + i * 3));
            if (!e)
                return NULL;
            asdl_seq_SET(handlers, i, e);
        }

        except_st = TryExcept(body, handlers, orelse, LINENO(n),
                              n->n_col_offset, c->c_arena);
        if (!finally)
            return except_st;

        /* if a 'finally' is present too, we nest the TryExcept within a
           TryFinally to emulate try ... except ... finally */
        body = asdl_seq_new(1, c->c_arena);
        if (body == NULL)
            return NULL;
        asdl_seq_SET(body, 0, except_st);
    }

    /* must be a try ... finally (except clauses are in body, if any exist) */
    assert(finally != NULL);
    return TryFinally(body, finally, LINENO(n), n->n_col_offset, c->c_arena);
}

static expr_ty
ast_for_with_var(struct compiling *c, const node *n)
{
    REQ(n, with_var);
    return ast_for_expr(c, CHILD(n, 1));
}

/* with_stmt: 'with' test [ with_var ] ':' suite */
static stmt_ty
ast_for_with_stmt(struct compiling *c, const node *n)
{
    expr_ty context_expr, optional_vars = NULL;
    int suite_index = 3;    /* skip 'with', test, and ':' */
    asdl_seq *suite_seq;

    assert(TYPE(n) == with_stmt);
    context_expr = ast_for_expr(c, CHILD(n, 1));
    if (!context_expr)
        return NULL;
    if (TYPE(CHILD(n, 2)) == with_var) {
        optional_vars = ast_for_with_var(c, CHILD(n, 2));

        if (!optional_vars) {
            return NULL;
        }
        if (!set_context(c, optional_vars, Store, n)) {
            return NULL;
        }
        suite_index = 4;
    }

    suite_seq = ast_for_suite(c, CHILD(n, suite_index));
    if (!suite_seq) {
        return NULL;
    }
    return With(context_expr, optional_vars, suite_seq, LINENO(n), 
                n->n_col_offset, c->c_arena);
}

static stmt_ty
ast_for_classdef(struct compiling *c, const node *n, asdl_seq *decorator_seq)
{
    /* classdef: 'class' NAME ['(' testlist ')'] ':' suite */
    PyObject *classname;
    asdl_seq *bases, *s;
    
    REQ(n, classdef);

    if (!forbidden_check(c, n, STR(CHILD(n, 1))))
            return NULL;

    if (NCH(n) == 4) {
        s = ast_for_suite(c, CHILD(n, 3));
        if (!s)
            return NULL;
        classname = NEW_IDENTIFIER(CHILD(n, 1));
        if (!classname)
            return NULL;
        return ClassDef(classname, NULL, s, decorator_seq, LINENO(n),
                        n->n_col_offset, c->c_arena);
    }
    /* check for empty base list */
    if (TYPE(CHILD(n,3)) == RPAR) {
        s = ast_for_suite(c, CHILD(n,5));
        if (!s)
            return NULL;
        classname = NEW_IDENTIFIER(CHILD(n, 1));
        if (!classname)
            return NULL;
        return ClassDef(classname, NULL, s, decorator_seq, LINENO(n),
                        n->n_col_offset, c->c_arena);
    }

    /* else handle the base class list */
    bases = ast_for_class_bases(c, CHILD(n, 3));
    if (!bases)
        return NULL;

    s = ast_for_suite(c, CHILD(n, 6));
    if (!s)
        return NULL;
    classname = NEW_IDENTIFIER(CHILD(n, 1));
    if (!classname)
        return NULL;
    return ClassDef(classname, bases, s, decorator_seq,
                    LINENO(n), n->n_col_offset, c->c_arena);
}

static stmt_ty
ast_for_stmt(struct compiling *c, const node *n)
{
    if (TYPE(n) == stmt) {
        assert(NCH(n) == 1);
        n = CHILD(n, 0);
    }
    if (TYPE(n) == simple_stmt) {
        assert(num_stmts(n) == 1);
        n = CHILD(n, 0);
    }
    if (TYPE(n) == small_stmt) {
        REQ(n, small_stmt);
        n = CHILD(n, 0);
        /* small_stmt: expr_stmt | print_stmt  | del_stmt | pass_stmt
                     | flow_stmt | import_stmt | global_stmt | exec_stmt
                     | assert_stmt
        */
        switch (TYPE(n)) {
            case expr_stmt:
                return ast_for_expr_stmt(c, n);
            case print_stmt:
                return ast_for_print_stmt(c, n);
            case del_stmt:
                return ast_for_del_stmt(c, n);
            case pass_stmt:
                return Pass(LINENO(n), n->n_col_offset, c->c_arena);
            case flow_stmt:
                return ast_for_flow_stmt(c, n);
            case import_stmt:
                return ast_for_import_stmt(c, n);
            case global_stmt:
                return ast_for_global_stmt(c, n);
            case exec_stmt:
                return ast_for_exec_stmt(c, n);
            case assert_stmt:
                return ast_for_assert_stmt(c, n);
            default:
                PyErr_Format(PyExc_SystemError,
                             "unhandled small_stmt: TYPE=%d NCH=%d\n",
                             TYPE(n), NCH(n));
                return NULL;
        }
    }
    else {
        /* compound_stmt: if_stmt | while_stmt | for_stmt | try_stmt
                        | funcdef | classdef | decorated
        */
        node *ch = CHILD(n, 0);
        REQ(n, compound_stmt);
        switch (TYPE(ch)) {
            case if_stmt:
                return ast_for_if_stmt(c, ch);
            case while_stmt:
                return ast_for_while_stmt(c, ch);
            case for_stmt:
                return ast_for_for_stmt(c, ch);
            case try_stmt:
                return ast_for_try_stmt(c, ch);
            case with_stmt:
                return ast_for_with_stmt(c, ch);
            case funcdef:
                return ast_for_funcdef(c, ch, NULL);
            case classdef:
                return ast_for_classdef(c, ch, NULL);
	    case decorated:
	        return ast_for_decorated(c, ch);
            default:
                PyErr_Format(PyExc_SystemError,
                             "unhandled small_stmt: TYPE=%d NCH=%d\n",
                             TYPE(n), NCH(n));
                return NULL;
        }
    }
}

static PyObject *
parsenumber(struct compiling *c, const char *s)
{
        const char *end;
        long x;
        double dx;
#ifndef WITHOUT_COMPLEX
        Py_complex complex;
        int imflag;
#endif

        errno = 0;
        end = s + strlen(s) - 1;
#ifndef WITHOUT_COMPLEX
        imflag = *end == 'j' || *end == 'J';
#endif
        if (*end == 'l' || *end == 'L')
                return PyLong_FromString((char *)s, (char **)0, 0);
        x = PyOS_strtol((char *)s, (char **)&end, 0);
        if (*end == '\0') {
                if (errno != 0)
                        return PyLong_FromString((char *)s, (char **)0, 0);
                return PyInt_FromLong(x);
        }
        /* XXX Huge floats may silently fail */
#ifndef WITHOUT_COMPLEX
        if (imflag) {
                complex.real = 0.;
                PyFPE_START_PROTECT("atof", return 0)
                complex.imag = PyOS_ascii_atof(s);
                PyFPE_END_PROTECT(complex)
                return PyComplex_FromCComplex(complex);
        }
        else
#endif
        {
                PyFPE_START_PROTECT("atof", return 0)
                dx = PyOS_ascii_atof(s);
                PyFPE_END_PROTECT(dx)
                return PyFloat_FromDouble(dx);
        }
}

static PyObject *
decode_utf8(struct compiling *c, const char **sPtr, const char *end, char* encoding)
{
#ifndef Py_USING_UNICODE
        Py_FatalError("decode_utf8 should not be called in this build.");
        return NULL;
#else
        PyObject *u, *v;
        char *s, *t;
        t = s = (char *)*sPtr;
        /* while (s < end && *s != '\\') s++; */ /* inefficient for u".." */
        while (s < end && (*s & 0x80)) s++;
        *sPtr = s;
        u = PyUnicode_DecodeUTF8(t, s - t, NULL);
        if (u == NULL)
                return NULL;
        v = PyUnicode_AsEncodedString(u, encoding, NULL);
        Py_DECREF(u);
        return v;
#endif
}

#ifdef Py_USING_UNICODE
static PyObject *
decode_unicode(struct compiling *c, const char *s, size_t len, int rawmode, const char *encoding)
{
        PyObject *v, *u;
        char *buf;
        char *p;
        const char *end;
        if (encoding == NULL) {
                buf = (char *)s;
                u = NULL;
        } else if (strcmp(encoding, "iso-8859-1") == 0) {
                buf = (char *)s;
                u = NULL;
        } else {
                /* check for integer overflow */
                if (len > PY_SIZE_MAX / 4)
                        return NULL;
                /* "\XX" may become "\u005c\uHHLL" (12 bytes) */
                u = PyString_FromStringAndSize((char *)NULL, len * 4);
                if (u == NULL)
                        return NULL;
                p = buf = PyString_AsString(u);
                end = s + len;
                while (s < end) {
                        if (*s == '\\') {
                                *p++ = *s++;
                                if (*s & 0x80) {
                                        strcpy(p, "u005c");
                                        p += 5;
                                }
                        }
                        if (*s & 0x80) { /* XXX inefficient */
                                PyObject *w;
                                char *r;
                                Py_ssize_t rn, i;
                                w = decode_utf8(c, &s, end, "utf-16-be");
                                if (w == NULL) {
                                        Py_DECREF(u);
                                        return NULL;
                                }
                                r = PyString_AsString(w);
                                rn = PyString_Size(w);
                                assert(rn % 2 == 0);
                                for (i = 0; i < rn; i += 2) {
                                        sprintf(p, "\\u%02x%02x",
                                                r[i + 0] & 0xFF,
                                                r[i + 1] & 0xFF);
                                        p += 6;
                                }
                                Py_DECREF(w);
                        } else {
                                *p++ = *s++;
                        }
                }
                len = p - buf;
                s = buf;
        }
        if (rawmode)
                v = PyUnicode_DecodeRawUnicodeEscape(s, len, NULL);
        else
                v = PyUnicode_DecodeUnicodeEscape(s, len, NULL);
        Py_XDECREF(u);
        return v;
}
#endif

/* s is a Python string literal, including the bracketing quote characters,
 * and r &/or u prefixes (if any), and embedded escape sequences (if any).
 * parsestr parses it, and returns the decoded Python string object.
 */
static PyObject *
parsestr(struct compiling *c, const char *s)
{
        size_t len;
        int quote = Py_CHARMASK(*s);
        int rawmode = 0;
        int need_encoding;
        int unicode = c->c_future_unicode;

        if (isalpha(quote) || quote == '_') {
                if (quote == 'u' || quote == 'U') {
                        quote = *++s;
                        unicode = 1;
                }
                if (quote == 'b' || quote == 'B') {
                        quote = *++s;
                        unicode = 0;
                }
                if (quote == 'r' || quote == 'R') {
                        quote = *++s;
                        rawmode = 1;
                }
        }
        if (quote != '\'' && quote != '\"') {
                PyErr_BadInternalCall();
                return NULL;
        }
        s++;
        len = strlen(s);
        if (len > INT_MAX) {
                PyErr_SetString(PyExc_OverflowError, 
                                "string to parse is too long");
                return NULL;
        }
        if (s[--len] != quote) {
                PyErr_BadInternalCall();
                return NULL;
        }
        if (len >= 4 && s[0] == quote && s[1] == quote) {
                s += 2;
                len -= 2;
                if (s[--len] != quote || s[--len] != quote) {
                        PyErr_BadInternalCall();
                        return NULL;
                }
        }
#ifdef Py_USING_UNICODE
        if (unicode || Py_UnicodeFlag) {
                return decode_unicode(c, s, len, rawmode, c->c_encoding);
        }
#endif
        need_encoding = (c->c_encoding != NULL &&
                         strcmp(c->c_encoding, "utf-8") != 0 &&
                         strcmp(c->c_encoding, "iso-8859-1") != 0);
        if (rawmode || strchr(s, '\\') == NULL) {
                if (need_encoding) {
#ifndef Py_USING_UNICODE
                        /* This should not happen - we never see any other
                           encoding. */
                        Py_FatalError(
                            "cannot deal with encodings in this build.");
#else
                        PyObject *v, *u = PyUnicode_DecodeUTF8(s, len, NULL);
                        if (u == NULL)
                                return NULL;
                        v = PyUnicode_AsEncodedString(u, c->c_encoding, NULL);
                        Py_DECREF(u);
                        return v;
#endif
                } else {
                        return PyString_FromStringAndSize(s, len);
                }
        }

        return PyString_DecodeEscape(s, len, NULL, unicode,
                                     need_encoding ? c->c_encoding : NULL);
}

/* Build a Python string object out of a STRING atom.  This takes care of
 * compile-time literal catenation, calling parsestr() on each piece, and
 * pasting the intermediate results together.
 */
static PyObject *
parsestrplus(struct compiling *c, const node *n)
{
        PyObject *v;
        int i;
        REQ(CHILD(n, 0), STRING);
        if ((v = parsestr(c, STR(CHILD(n, 0)))) != NULL) {
                /* String literal concatenation */
                for (i = 1; i < NCH(n); i++) {
                        PyObject *s;
                        s = parsestr(c, STR(CHILD(n, i)));
                        if (s == NULL)
                                goto onError;
                        if (PyString_Check(v) && PyString_Check(s)) {
                                PyString_ConcatAndDel(&v, s);
                                if (v == NULL)
                                    goto onError;
                        }
#ifdef Py_USING_UNICODE
                        else {
                                PyObject *temp = PyUnicode_Concat(v, s);
                                Py_DECREF(s);
                                Py_DECREF(v);
                                v = temp;
                                if (v == NULL)
                                    goto onError;
                        }
#endif
                }
        }
        return v;

 onError:
        Py_XDECREF(v);
        return NULL;
}
