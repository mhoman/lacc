#define _XOPEN_SOURCE 500 /* snprintf */
#include "symtab.h"
#include "typetree.h"
#include <lacc/context.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct namespace
    ns_ident = {"identifiers"},
    ns_label = {"labels"},
    ns_tag = {"tags"};

/* Name prefixes assigned to compiler generated symbols. */
#define PREFIX_TEMPORARY ".t"
#define PREFIX_UNNAMED ".u"
#define PREFIX_CONSTANT ".C"
#define PREFIX_STRING ".LC"
#define PREFIX_LABEL ".L"

/*
 * Maintain list of symbols allocated for temporaries and labels, which
 * can be reused between function definitions.
 *
 * Calling sym_discard will push symbols back into this list.
 */
static array_of(struct symbol *) temporaries;

static struct symbol *alloc_sym(void)
{
    struct symbol *sym;

    if (array_len(&temporaries)) {
        sym = array_pop_back(&temporaries);
        memset(sym, 0, sizeof(*sym));
    } else {
        sym = calloc(1, sizeof(*sym));
    }

    return sym;
}

/* Save memcpy reference for backend. */
const struct symbol *decl_memcpy = NULL;

/*
 * Keep track of all function declarations globally, in order to coerce
 * forward declarations made in inner scope.
 *
 * int foo(void) {
 *     int bar(int);
 *     return bar(42);
 * }
 *
 * int bar(int a) {
 *     return a * a;
 * }
 *
 * In the above example, both references to bar must resolve to the same
 * symbol, even though the first declaration is not in scope for the
 * actual definition.
 */
static struct hash_table functions;

static String sym_hash_key(void *ref)
{
    return ((const struct symbol *) ref)->name;
}

static struct symbol *sym_lookup_function(String name)
{
    static int init;

    if (!init) {
        hash_init(&functions, 1024, &sym_hash_key, NULL, NULL);
        init = 1;
    }

    return hash_lookup(&functions, name);
}

static void sym_clear_buffers(void)
{
    int i;
    struct symbol *sym;

    for (i = 0; i < array_len(&temporaries); ++i) {
        sym = array_get(&temporaries, i);
        free(sym);
    }

    array_clear(&temporaries);
    hash_destroy(&functions);
}

/*
 * Initialize hash table with initial size heuristic based on scope
 * depth. As a special case, depth 1 containing function arguments is
 * assumed to contain fewer symbols.
 */
static unsigned current_scope_hash_cap(struct namespace *ns)
{
    static const unsigned hash_cap[] = {256, 16, 128, 64, 32, 16};
    static const unsigned hash_cap_default = 8;

    unsigned cap;
    assert(array_len(&ns->scope));
    cap = hash_cap_default;
    if (array_len(&ns->scope) < sizeof(hash_cap) / sizeof(hash_cap[0])) {
        cap = hash_cap[array_len(&ns->scope) - 1];
    }

    return cap;
}

void push_scope(struct namespace *ns)
{
    static struct scope empty;
    struct scope *scope;

    if (array_len(&ns->scope) < ns->max_scope_depth) {
        assert(array_len(&ns->scope) < ns->scope.capacity);
        array_len(&ns->scope) += 1;
        scope = &array_get(&ns->scope, array_len(&ns->scope) - 1);
        if (scope->state == SCOPE_INITIALIZED) {
            scope->state = SCOPE_DIRTY;
        }
    } else {
        ns->max_scope_depth += 1;
        array_push_back(&ns->scope, empty);
        scope = &array_get(&ns->scope, array_len(&ns->scope) - 1);
        scope->state = SCOPE_CREATED;
    }
}

void pop_scope(struct namespace *ns)
{
    int i;
    struct symbol *sym;
    struct scope *scope;

    /*
     * Popping last scope frees the whole symbol table, including the
     * symbols themselves. For label scope, which is per function, make
     * sure there are no tentative definitions.
     */
    assert(array_len(&ns->scope) > 0);
    if (array_len(&ns->scope) == 1) {
        for (i = 0; i < ns->max_scope_depth; ++i) {
            scope = &array_get(&ns->scope, i);
            if (scope->state != SCOPE_CREATED) {
                hash_destroy(&scope->table);
            }
        }

        ns->max_scope_depth = 0;
        array_clear(&ns->scope);
        for (i = 0; i < array_len(&ns->symbol); ++i) {
            sym = array_get(&ns->symbol, i);
            if (ns == &ns_label && sym->symtype == SYM_TENTATIVE) {
                error("Undefined label '%s'.", sym_name(sym));
            }
            free(sym);
        }

        array_clear(&ns->symbol);

        /*
         * Temporaries should only be freed once, at exit. Check for a
         * particular namespace that is only popped completely at the
         * end of the translation unit.
         */
        if (ns == &ns_ident) {
            sym_clear_buffers();
        }
    } else {
        array_len(&ns->scope) -= 1;
    }
}

unsigned current_scope_depth(struct namespace *ns)
{
    unsigned depth = array_len(&ns->scope);
    assert(depth);
    return depth - 1;
}

struct symbol *sym_lookup(struct namespace *ns, String name)
{
    int i;
    struct scope *scope;
    struct symbol *sym;

    for (i = array_len(&ns->scope) - 1; i >= 0; --i) {
        scope = &array_get(&ns->scope, i);
        if (scope->state == SCOPE_INITIALIZED) {
            sym = hash_lookup(&scope->table, name);
            if (sym) {
                sym->referenced = 1;
                return sym;
            }
        }
    }

    return NULL;
}

const char *sym_name(const struct symbol *sym)
{
    static char name[128];

    if (!sym->n)
        return str_raw(sym->name);

    /*
     * Temporary variables and string literals are named '.t' and '.LC',
     * respectively. For those, append the numeral without anything in
     * between. For other variables, which are disambiguated statics,
     * insert a period between the name and the number.
     */
    if (str_raw(sym->name)[0] == '.')
        snprintf(name, sizeof(name), "%s%d", str_raw(sym->name), sym->n);
    else
        snprintf(name, sizeof(name), "%s.%d", str_raw(sym->name), sym->n);

    return name;
}

/*
 * Symbols can be declared multiple times, with incomplete or complete
 * types. Only functions and arrays can exist as incomplete. Other
 * symbols can be re-declared, but must have identical type each time.
 *
 * For functions, the last parameter list is applied for as long as the
 * symbol is still tentative.
 */
static void apply_type(struct symbol *sym, Type type)
{
    int conflict = 1;

    if (type_equal(sym->type, type)
        && !(is_function(sym->type) && sym->symtype != SYM_DEFINITION))
        return;

    switch (type_of(sym->type)) {
    case T_FUNCTION:
        if (is_function(type)
            && type_equal(type_next(sym->type), type_next(type)))
        {
            conflict = nmembers(sym->type) != nmembers(type);
            if (!conflict) {
                sym->type = type;
            }
        }
        break;
    case T_ARRAY:
        if (is_array(type)
            && type_equal(type_next(sym->type), type_next(type)))
        {
            conflict = 0;
            if (!size_of(sym->type)) {
                assert(size_of(type));
                set_array_length(sym->type, type_array_len(type));
            }
        }
    default: break;
    }

    if (conflict) {
        error("Incompatible declaration of %s :: %t, cannot apply type '%t'.",
            str_raw(sym->name), sym->type, type);
        exit(1);
    }
}

void sym_make_visible(struct namespace *ns, struct symbol *sym)
{
    unsigned cap;
    struct scope *scope;

    scope = &array_get(&ns->scope, array_len(&ns->scope) - 1);
    switch (scope->state) {
    case SCOPE_CREATED:
        cap = current_scope_hash_cap(ns);
        hash_init(&scope->table, cap, &sym_hash_key, NULL, NULL);
        break;
    case SCOPE_DIRTY:
        hash_clear(&scope->table);
        break;
    default: break;
    }

    hash_insert(&scope->table, (void *) sym);
    scope->state = SCOPE_INITIALIZED;
}

struct symbol *sym_add(
    struct namespace *ns,
    String name,
    Type type,
    enum symtype symtype,
    enum linkage linkage)
{
    static int n;

    struct symbol *sym = NULL;
    assert(symtype != SYM_LABEL);
    assert(symtype != SYM_TAG || ns == &ns_tag);

    /* All function declarations must agree, regardless of scope. */
    if (symtype != SYM_STRING_VALUE) {
        sym = sym_lookup(ns, name);
        if (!sym && is_function(type)) {
            assert(ns == &ns_ident);
            sym = sym_lookup_function(name);
            if (sym) {
                apply_type(sym, type);
                sym_make_visible(ns, sym);
                if (current_scope_depth(ns) < sym->depth) {
                    sym->depth = current_scope_depth(ns);
                }
                return sym;
            }
        }
    }

    /* Try to complete existing tentative definition. */
    if (sym) {
        if (linkage == LINK_EXTERN && symtype == SYM_DECLARATION
            && (sym->symtype == SYM_TENTATIVE
                || sym->symtype == SYM_DEFINITION))
        {
            apply_type(sym, type);
            return sym;
        } else if (sym->depth == current_scope_depth(ns) && !sym->depth) {
            if (sym->linkage == linkage
                && ((sym->symtype == SYM_TENTATIVE
                        && symtype == SYM_DEFINITION)
                    || (sym->symtype == SYM_DEFINITION
                        && symtype == SYM_TENTATIVE)))
            {
                apply_type(sym, type);
                sym->symtype = SYM_DEFINITION;
            } else if (
                sym->linkage == linkage
                && sym->symtype == SYM_DECLARATION
                && symtype == SYM_TENTATIVE)
            {
                apply_type(sym, type);
                sym->symtype = SYM_TENTATIVE;
            } else if (
                sym->linkage == linkage
                && sym->symtype == SYM_DEFINITION
                && symtype == SYM_DECLARATION)
            {
                if (!type_equal(sym->type, type)) {
                    error("Conflicting types for %s.", str_raw(name));
                    exit(1);
                }
            } else if (sym->symtype != symtype || sym->linkage != linkage) {
                error("Declaration of '%s' does not match prior declaration.",
                    str_raw(name));
                exit(1);
            } else {
                apply_type(sym, type);
            }
            return sym;
        } else if (sym->depth == current_scope_depth(ns) && sym->depth) {
            error("Duplicate definition of symbol '%s'.", str_raw(name));
            exit(1);
        }
    }

    /* Create new symbol. */
    sym = alloc_sym();
    sym->depth = current_scope_depth(ns);
    sym->name = name;
    sym->type = type;
    sym->symtype = symtype;
    sym->linkage = linkage;
    if (!decl_memcpy && !str_cmp(str_init("memcpy"), sym->name)) {
        decl_memcpy = sym;
    }

    /*
     * Scoped static variable are given unique names in order to not
     * collide with other external declarations.
     */
    if (linkage == LINK_INTERN && sym->depth) {
        sym->n = ++n;
    }

    if (sym->symtype == SYM_TAG || sym->symtype == SYM_TYPEDEF) {
        type_set_tag(type, sym);
    }

    array_push_back(&ns->symbol, sym);
    sym_make_visible(ns, sym);
    if (is_function(sym->type)) {
        hash_insert(&functions, sym);
    }

    verbose(
        "\t[type: %s, link: %s]\n"
        "\t%s :: %t",
        (sym->symtype == SYM_DEFINITION ? "definition" :
            sym->symtype == SYM_TENTATIVE ? "tentative" :
            sym->symtype == SYM_DECLARATION ? "declaration" :
            sym->symtype == SYM_TYPEDEF ? "typedef" :
            sym->symtype == SYM_TAG ? "tag" :
            sym->symtype == SYM_CONSTANT ? "number" : "string"),
        (sym->linkage == LINK_INTERN ? "intern" :
            sym->linkage == LINK_EXTERN ? "extern" : "none"),
        sym_name(sym),
        sym->type);

    return sym;
}

struct symbol *sym_create_temporary(Type type)
{
    static int n;
    struct symbol *sym;

    sym = alloc_sym();
    sym->symtype = SYM_DEFINITION;
    sym->linkage = LINK_NONE;
    sym->name = str_init(PREFIX_TEMPORARY);
    sym->type = type;
    sym->n = ++n;
    return sym;
}

struct symbol *sym_create_unnamed(Type type)
{
    static int n;
    struct symbol *sym;

    sym = alloc_sym();
    if (current_scope_depth(&ns_ident) == 0) {
        sym->linkage = LINK_INTERN;
    } else {
        sym->linkage = LINK_NONE;
    }

    sym->symtype = SYM_DEFINITION;
    sym->name = str_init(PREFIX_UNNAMED);
    sym->type = type;
    sym->n = ++n;
    return sym;
}

struct symbol *sym_create_label(void)
{
    static int n;
    struct symbol *sym;

    sym = alloc_sym();
    sym->type = basic_type__void;
    sym->symtype = SYM_LABEL;
    sym->linkage = LINK_INTERN;
    sym->name = str_init(PREFIX_LABEL);
    sym->n = ++n;
    return sym;
}

struct symbol *sym_create_constant(Type type, union value val)
{
    static int n;
    struct symbol *sym;

    sym = alloc_sym();
    sym->type = type;
    sym->value.constant = val;
    sym->symtype = SYM_CONSTANT;
    sym->linkage = LINK_INTERN;
    sym->name = str_init(PREFIX_CONSTANT);
    sym->n = ++n;
    array_push_back(&ns_ident.symbol, sym);
    return sym;
}

/*
 * Store string value directly on symbol, memory ownership is in string
 * table from previously called str_register. The symbol now exists as
 * if declared static char .LC[] = "...".
 */
struct symbol *sym_create_string(String str)
{
    static int n;
    struct symbol *sym;

    sym = alloc_sym();
    sym->type =
        type_create(T_ARRAY, basic_type__char, (size_t) str.len + 1, NULL);
    sym->value.string = str;
    sym->symtype = SYM_STRING_VALUE;
    sym->linkage = LINK_INTERN;
    sym->name = str_init(PREFIX_STRING);
    sym->n = ++n;
    array_push_back(&ns_ident.symbol, sym);
    return sym;
}

void sym_discard(struct symbol *sym)
{
    array_push_back(&temporaries, sym);
}

int is_temporary(const struct symbol *sym)
{
    const char *raw = str_raw(sym->name);
    return strcmp(PREFIX_TEMPORARY, raw) == 0;
}

const struct symbol *yield_declaration(struct namespace *ns)
{
    const struct symbol *sym;

    while (ns->cursor < array_len(&ns->symbol)) {
        sym = array_get(&ns->symbol, ns->cursor);
        ns->cursor++;
        switch (sym->symtype) {
        case SYM_TENTATIVE:
        case SYM_STRING_VALUE:
            break;
        case SYM_CONSTANT:
            if (is_real(sym->type)) {
                break;
            }
            continue;
        case SYM_DECLARATION:
            if (sym->linkage == LINK_EXTERN
                && (sym->referenced || sym == decl_memcpy))
            {
                break;
            }
        default:
            continue;
        }
        return sym;
    }

    return NULL;
}

static void print_symbol(FILE *stream, const struct symbol *sym)
{
    fprintf(stream, "%*s", sym->depth * 2, "");
    if (sym->linkage != LINK_NONE) {
        fprintf(stream, "%s ",
            (sym->linkage == LINK_INTERN) ? "static" : "global");
    }

    switch (sym->symtype) {
    case SYM_TENTATIVE:
        fprintf(stream, "tentative ");
        break;
    case SYM_DEFINITION:
        fprintf(stream, "definition ");
        break;
    case SYM_DECLARATION:
        fprintf(stream, "declaration ");
        break;
    case SYM_TYPEDEF:
        fprintf(stream, "typedef ");
        break;
    case SYM_TAG:
        if (is_struct(sym->type)) {
            fprintf(stream, "struct ");
        } else if (is_union(sym->type)) {
            fprintf(stream, "union ");
        } else {
            assert(type_equal(basic_type__int, sym->type));
            fprintf(stream, "enum ");
        }
        break;
    case SYM_CONSTANT:
        fprintf(stream, "number ");
        break;
    case SYM_STRING_VALUE:
        fprintf(stream, "string ");
        break;
    case SYM_LABEL:
        fprintf(stream, "label ");
        break;
    }

    fprintf(stream, "%s :: ", sym_name(sym));
    fprinttype(stream, sym->type, sym);
    if (size_of(sym->type)) {
        fprintf(stream, ", size=%lu", size_of(sym->type));
    }

    if (sym->stack_offset) {
        fprintf(stream, ", (stack_offset: %d)", sym->stack_offset);
    }
    if (is_vla(sym->type)) {
        fprintf(stream, ", (vla_address: %s)",
            sym_name(sym->value.vla_address));
    }

    if (sym->symtype == SYM_CONSTANT) {
        if (is_signed(sym->type)) {
            fprintf(stream, ", value=%ld", sym->value.constant.i);
        } else if (is_unsigned(sym->type)) {
            fprintf(stream, ", value=%lu", sym->value.constant.u);
        } else if (is_float(sym->type)) {
            fprintf(stream, ", value=%ff", sym->value.constant.f);
        } else if (is_double(sym->type)) {
            fprintf(stream, ", value=%f", sym->value.constant.d);
        } else {
            assert(is_long_double(sym->type));
            fprintf(stream, ", value=%Lf", sym->value.constant.ld);
        }
    }
}

void output_symbols(FILE *stream, struct namespace *ns)
{
    int i;
    const struct symbol *sym;

    for (i = 0; i < array_len(&ns->symbol); ++i) {
        if (!i) {
            fprintf(stream, "namespace %s:\n", ns->name);
        }

        sym = array_get(&ns->symbol, i);
        print_symbol(stream, sym);
        fprintf(stream, "\n");
    }
}
