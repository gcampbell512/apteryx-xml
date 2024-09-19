/**
 * @file sch_xpath.c
 * XPATH parsing utilities
 *
 * This code is a translation of a C# project https://github.com/quamotion/XPathParser and carries the project license agreement below
 *
 * Microsoft Public License (Ms-PL)
 *
 * This license governs use of the accompanying software. If you use the software, you accept this license. If you do not accept the license, do not use the software.
 * 1. Definitions
 * The terms "reproduce," "reproduction," "derivative works," and "distribution" have the same meaning here as under U.S. copyright law.
 * A "contribution" is the original software, or any additions or changes to the software.
 * A "contributor" is any person that distributes its contribution under this license.
 * "Licensed patents" are a contributor's patent claims that read directly on its contribution.
 * 2. Grant of Rights
 * (A) Copyright Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide,
 *     royalty-free copyright license to reproduce its contribution, prepare derivative works of its contribution, and distribute its contribution or any derivative works that you create.
 * (B) Patent Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide,
 *     royalty-free license under its licensed patents to make, have made, use, sell, offer for sale, import, and/or otherwise dispose of its contribution in the software or derivative works of the contribution in the software.
 * 3. Conditions and Limitations
 * (A) No Trademark License- This license does not grant you rights to use any contributors' name, logo, or trademarks.
 * (B) If you bring a patent claim against any contributor over patents that you claim are infringed by the software, your patent license from such contributor to the software ends automatically.
 * (C) If you distribute any portion of the software, you must retain all copyright, patent, trademark, and attribution notices that are present in the software.
 * (D) If you distribute any portion of the software in source code form, you may do so only under this license by including a complete copy of this license with your distribution. If you distribute
 *     any portion of the software in compiled or object code form, you may only do so under a license that complies with this license.
 * (E) The software is licensed "as-is." You bear the risk of using it. The contributors give no express warranties, guarantees or conditions. You may have additional consumer rights under your local
 *     laws which this license cannot change. To the extent permitted under your local laws, the contributors exclude the implied warranties of merchantability, fitness for a particular purpose and non-infringement.
 */

#include "sch_xpath.h"
#include <glib/gregex.h>

/* Debug */
#define DEBUG(fmt, args...) \
    if (xpath_debug || xpath_verbose) \
    { \
        syslog (LOG_DEBUG, fmt, ## args); \
        printf (fmt, ## args); \
    }
#define VERBOSE(fmt, args...) \
    if (xpath_verbose) \
    { \
        syslog (LOG_DEBUG, fmt, ## args); \
        printf (fmt, ## args); \
    }
#define NOTICE(fmt, args...) \
    { \
        syslog (LOG_NOTICE, fmt, ## args); \
        printf (fmt, ## args); \
    }
#define ERROR(fmt, args...) \
    { \
        syslog (LOG_ERR, fmt, ## args); \
        fprintf (stderr, fmt, ## args); \
    }

#ifndef G_REGEX_MATCH_DEFAULT
#define G_REGEX_MATCH_DEFAULT 0
#endif

#ifndef G_REGEX_DEFAULT
#define G_REGEX_DEFAULT 0
#endif

enum
{
    XPATH_KIND_UNKNOWN,
    XPATH_KIND_OR,
    XPATH_KIND_AND,
    XPATH_KIND_EQUAL,
    XPATH_KIND_NOT_EQUAL,
    XPATH_KIND_LESS_THAN,
    XPATH_KIND_LESS_EQUAL,
    XPATH_KIND_GREATER_THAN,
    XPATH_KIND_GREATER_EQUAL,
    XPATH_KIND_PLUS,
    XPATH_KIND_MINUS,
    XPATH_KIND_MULTIPLY,
    XPATH_KIND_DIVIDE,
    XPATH_KIND_MODULO,
    XPATH_KIND_UNION,
    XPATH_KIND_DOTDOT,
    XPATH_KIND_COLONCOLON,
    XPATH_KIND_SLASHSLASH,
    XPATH_KIND_NUMBER,
    XPATH_KIND_AXIS,
    XPATH_KIND_NAME,
    XPATH_KIND_STRING,
    XPATH_KIND_EOF,
    XPATH_KIND_LPARENS,
    XPATH_KIND_RPARENS,
    XPATH_KIND_LBRACKET,
    XPATH_KIND_RBRACKET,
    XPATH_KIND_DOT,
    XPATH_KIND_AT,
    XPATH_KIND_COMMA,
    XPATH_KIND_STAR,
    XPATH_KIND_SLASH,
    XPATH_KIND_DOLLAR,
    XPATH_KIND_RBRACE,
};

#define XPATH_KIND_LAST_OPERATOR XPATH_KIND_UNION
#define XPATH_KIND_FIRST_STRINGABLE XPATH_KIND_NAME
#define XPATH_KIND_LAST_NON_CHAR XPATH_KIND_EOF

typedef struct _xpath_scanner
{
    char *xpath_expr;
    int xpath_expr_len;
    int cur_index;
    char cur_char;
    int kind;
    char *name;
    char *prefix;
    char *string_value;
    bool can_be_function;
    int start;
    int prev_end;
    int prev_kind;
    int prev_len_end;
    xpath_axis axis;
} xpath_scanner;

typedef struct _xpath_pos
{
    int start_char;
    int end_char;
} xpath_pos;

static int xpath_axis_types[] = {
    XPATH_TYPE_UNKNOWN,
    XPATH_TYPE_ANCESTOR,
    XPATH_TYPE_ANCESTOR_OR_SELF,
    XPATH_TYPE_ATTRIBUTE,
    XPATH_TYPE_CHILD,
    XPATH_TYPE_DESCENDANT,
    XPATH_TYPE_DESCENDANT_OR_SELF,
    XPATH_TYPE_FOLLOWING,
    XPATH_TYPE_FOLLOWING_SIBLING,
    XPATH_TYPE_NAMESPACE,
    XPATH_TYPE_PARENT,
    XPATH_TYPE_PRECEDING,
    XPATH_TYPE_PRECEDING_SIBLING,
    XPATH_TYPE_SELF,
    XPATH_TYPE_ROOT,
};

static int num_axis_types = sizeof (xpath_axis_types) / sizeof (int);

static int xpath_oper_types[] = {
    XPATH_TYPE_UNKNOWN,
    XPATH_TYPE_OR,
    XPATH_TYPE_AND,
    XPATH_TYPE_EQ,
    XPATH_TYPE_NE,
    XPATH_TYPE_LT,
    XPATH_TYPE_LE,
    XPATH_TYPE_GT,
    XPATH_TYPE_GE,
    XPATH_TYPE_PLUS,
    XPATH_TYPE_MINUS,
    XPATH_TYPE_MULTIPLY,
    XPATH_TYPE_DIVIDE,
    XPATH_TYPE_MODULO,
    XPATH_TYPE_UNARY_MINUS,
    XPATH_TYPE_UNION,
};

static int num_oper_types = sizeof (xpath_oper_types) / sizeof (int);

static char *xpath_node_type_strings[] = {
    "Unknown",
    "All",
    "Text",
    "Processing-instruction",
    "Comment",
    "Attribute",
    "Namespace",
};

static int num_nt_strings = sizeof (xpath_node_type_strings) / sizeof (char *);

static int xpath_operator_precedence[] = {
    /*Unknown    */ 0,
    /*Or         */ 1,
    /*And        */ 2,
    /*Eq         */ 3,
    /*Ne         */ 3,
    /*Lt         */ 4,
    /*Le         */ 4,
    /*Gt         */ 4,
    /*Ge         */ 4,
    /*Plus       */ 5,
    /*Minus      */ 5,
    /*Multiply   */ 6,
    /*Divide     */ 6,
    /*Modulo     */ 6,
    /*UnaryMinus */ 7,
    /*Union      */ 8,
    // Not used
};

static bool xpath_debug = false;
static bool xpath_verbose = false;

static xpath_funcs bld_funcs = { };

static void
sch_xpath_free_node (xpath_node *opnd)
{
    if (opnd)
    {
        g_free (opnd->node_type);
        g_free (opnd->string_value);
        g_free (opnd->number);
        g_free (opnd->prefix);
        g_free (opnd->name);
        g_free (opnd->axis);
        g_list_free (opnd->arg_list);
        g_free (opnd);
    }
}

xpath_node *
sch_xpath_allocate_node (void)
{
    return g_malloc0 (sizeof (xpath_node));
}

static void
sch_xpath_free_scanner (xpath_scanner *xpar)
{
    /* Note xpar->xpath_expr still belongs to the caller*/
    g_free (xpar->name);
    g_free (xpar->prefix);
    g_free (xpar->string_value);
    g_free (xpar);
}

// May be called for the following tokens: Name, String, Eof, Comma, LParens, RParens, LBracket, RBracket, RBrace
static char *
sch_xpath_kind_to_string (int kind)
{
    if (kind < XPATH_KIND_FIRST_STRINGABLE)
    {
        DEBUG ("%s:%u Invalid kind %d to make into string\n", __func__, __LINE__, kind);
        return NULL;
    }

    if (XPATH_KIND_LAST_NON_CHAR < kind)
    {
        switch (kind)
        {
        case XPATH_KIND_LPARENS:
            return "(";
        case XPATH_KIND_RPARENS:
            return ")";
        case XPATH_KIND_LBRACKET:
            return "[";
        case XPATH_KIND_RBRACKET:
            return "]";
        case XPATH_KIND_DOT:
            return ".";
        case XPATH_KIND_AT:
            return "@";
        case XPATH_KIND_COMMA:
            return ",";
        case XPATH_KIND_STAR:
            return "*";
        case XPATH_KIND_SLASH:
            return "/";
        case XPATH_KIND_DOLLAR:
            return "$";
        case XPATH_KIND_RBRACE:
            return "}";
        default:
            return NULL;
        }
    }

    switch (kind)
    {
    case XPATH_KIND_NAME:
        return "<name>";
    case XPATH_KIND_STRING:
        return "<string literal>";
    case XPATH_KIND_EOF:
        return "<eof>";
    default:
        DEBUG ("%s:%u Unexpected kind %d\n", __func__, __LINE__, kind);
        return NULL;
    }
}

static void
sch_xpath_next_char (xpath_scanner *xpar)
{
    if (xpar->cur_index < -1 || xpar->cur_index >= xpar->xpath_expr_len)
    {
        DEBUG ("%s:%u Invalid cur_index %d max %d\n", __func__, __LINE__,
               xpar->cur_index, xpar->xpath_expr_len);
        xpar->cur_char = '\0';
    }
    else
    {
        xpar->cur_index++;
        if (xpar->cur_index <= xpar->xpath_expr_len)
        {
            xpar->cur_char = xpar->xpath_expr[xpar->cur_index];
        }
    }
}

static void
sch_xpath_set_source_index (xpath_scanner *xpar, int index)
{
    if (index < 0 || index >= xpar->xpath_expr_len)
    {
        DEBUG ("%s:%u Invalid index %d max %d\n", __func__, __LINE__,
               index, xpar->xpath_expr_len);
    }
    else
    {
        xpar->cur_index = index - 1;
        sch_xpath_next_char (xpar);
    }
}

static char *
sch_xpath_name (xpath_scanner *xpar)
{
    if (xpar->kind != XPATH_KIND_NAME || xpar->name == NULL)
    {
        DEBUG ("%s:%u Invalid name - %d  %p\n", __func__, __LINE__, xpar->kind, xpar->name);
        g_free (xpar->name);
        return NULL;
    }
    return xpar->name;
}

static char *
sch_xpath_prefix (xpath_scanner *xpar)
{
    if (xpar->kind != XPATH_KIND_NAME || xpar->prefix == NULL)
    {
        DEBUG ("%s:%u Invalid prefix - %d  %p\n", __func__, __LINE__,
               xpar->kind, xpar->prefix);
        g_free (xpar->prefix);
        return NULL;
    }
    return xpar->prefix;
}

static char *
sch_xpath_raw_value (xpath_scanner *xpar)
{
    if (xpar->kind == XPATH_KIND_EOF)
        return g_strdup (sch_xpath_kind_to_string (xpar->kind));
    else
        return g_strndup (&xpar->xpath_expr[xpar->start], xpar->cur_index - xpar->start);
}

static char *
sch_xpath_string_value (xpath_scanner *xpar)
{
    if (xpar->kind == XPATH_KIND_STRING && xpar->string_value)
        return xpar->string_value;

    DEBUG ("%s:%u Invalid string - %d  %p\n", __func__, __LINE__,
            xpar->kind, xpar->string_value);
    g_free (xpar->string_value);
    return NULL;
}

// Returns true if the character following an QName (possibly after intervening
// ExprWhitespace) is '('. In this case the token must be recognized as a NodeType
// or a FunctionName unless it is an OperatorName. This distinction cannot be done
// without knowing the previous lexeme. For example, "or" in "... or (1 != 0)" may
// be an OperatorName or a FunctionName.
static bool
sch_xpath_can_be_function (xpath_scanner *xpar)
{
    if (xpar->kind != XPATH_KIND_NAME)
    {
        DEBUG ("%s:%u Invalid kind - %d\n", __func__, __LINE__, xpar->kind);
        return false;
    }
    return xpar->can_be_function;
}

static bool
sch_xpath_is_white_space (char ch)
{
    return ch == ' ';
}

static bool
sch_xpath_is_ascii_digit (char ch)
{
    return (uint) (ch - '0') <= 9;
}

static void
sch_xpath_scan_number (xpath_scanner *xpar)
{
    if (sch_xpath_is_ascii_digit (xpar->cur_char) || xpar->cur_char == '.')
    {
        while (sch_xpath_is_ascii_digit (xpar->cur_char))
            sch_xpath_next_char (xpar);

        if (xpar->cur_char == '.')
        {
            sch_xpath_next_char (xpar);
            while (sch_xpath_is_ascii_digit (xpar->cur_char))
                sch_xpath_next_char (xpar);
        }
        if ((xpar->cur_char & (~0x20)) == 'E')
        {
            sch_xpath_next_char (xpar);
            if (xpar->cur_char == '+' || xpar->cur_char == '-')
                sch_xpath_next_char (xpar);

            while (sch_xpath_is_ascii_digit (xpar->cur_char))
                sch_xpath_next_char (xpar);
            DEBUG ("%s:%u Invalid exponental number\n", __func__, __LINE__);
        }
    }
}

static bool
sch_xpath_check_operator (xpath_scanner *xpar, bool star)
{
    int op_kind;

    if (star)
    {
        op_kind = XPATH_KIND_MULTIPLY;
    }
    else
    {
        if (strlen (xpar->prefix) != 0 || strlen (xpar->name) > 3)
            return false;

        if (g_strcmp0 (xpar->name, "or") == 0)
            op_kind = XPATH_KIND_OR;
        else if (g_strcmp0 (xpar->name, "and") == 0)
            op_kind = XPATH_KIND_AND;
        else if (g_strcmp0 (xpar->name, "div") == 0)
            op_kind = XPATH_KIND_DIVIDE;
        else if (g_strcmp0 (xpar->name, "mod") == 0)
            op_kind = XPATH_KIND_MODULO;
        else
            return false;
    }

    // If there is a preceding token and the preceding token is not one of '@', '::', '(', '[', ',' or an Operator,
    // then a '*' must be recognized as a MultiplyOperator and an NCName must be recognized as an OperatorName.
    if (xpar->prev_kind <= XPATH_KIND_LAST_OPERATOR)
        return false;

    switch (xpar->prev_kind)
    {
    case XPATH_KIND_SLASH:
    case XPATH_KIND_SLASHSLASH:
    case XPATH_KIND_AT:
    case XPATH_KIND_COLONCOLON:
    case XPATH_KIND_LPARENS:
    case XPATH_KIND_LBRACKET:
    case XPATH_KIND_COMMA:
    case XPATH_KIND_DOLLAR:
        return false;
    }

    xpar->kind = op_kind;
    return true;
}

static xpath_axis
sch_xpath_check_axis (xpath_scanner *xpar)
{
    xpar->kind = XPATH_KIND_AXIS;
    if (g_strcmp0 (xpar->name, "ancestor") == 0)
        return XPATH_AXIS_ANCESTOR;
    else if (g_strcmp0 (xpar->name, "ancestor-or-self") == 0)
        return XPATH_AXIS_ANCESTOR_OR_SELF;
    else if (g_strcmp0 (xpar->name, "attribute") == 0)
        return XPATH_AXIS_ATTRIBUTE;
    else if (g_strcmp0 (xpar->name, "child") == 0)
        return XPATH_AXIS_CHILD;
    else if (g_strcmp0 (xpar->name, "descendant") == 0)
        return XPATH_AXIS_DESCENDANT;
    else if (g_strcmp0 (xpar->name, "descendant-or-self") == 0)
        return XPATH_AXIS_DESCENDANT_OR_SELF;
    else if (g_strcmp0 (xpar->name, "following") == 0)
        return XPATH_AXIS_FOLLOWING;
    else if (g_strcmp0 (xpar->name, "following-sibling") == 0)
        return XPATH_AXIS_FOLLOWING_SIBLING;
    else if (g_strcmp0 (xpar->name, "namespace") == 0)
        return XPATH_AXIS_NAMESPACE;
    else if (g_strcmp0 (xpar->name, "parent") == 0)
        return XPATH_AXIS_PARENT;
    else if (g_strcmp0 (xpar->name, "preceding") == 0)
        return XPATH_AXIS_PRECEDING;
    else if (g_strcmp0 (xpar->name, "preceding-sibling") == 0)
        return XPATH_AXIS_PRECEDING_SIBLING;
    else if (g_strcmp0 (xpar->name, "self") == 0)
        return XPATH_AXIS_SELF;

    xpar->kind = XPATH_KIND_NAME;
    return XPATH_AXIS_UNKNOWN;
}

static void
sch_xpath_scan_string (xpath_scanner *xpar)
{
    int start_idx = xpar->cur_index + 1;
    int end_idx;
    char *end = strchr (&xpar->xpath_expr[start_idx], xpar->cur_char);
    if (!end)
    {
        sch_xpath_set_source_index (xpar, xpar->xpath_expr_len);
        DEBUG ("%s:%u Unterminated xpath string\n", __func__, __LINE__);
        return;
    }

    end_idx = end - &xpar->xpath_expr[start_idx] + start_idx;
    xpar->string_value = g_strndup (&xpar->xpath_expr[start_idx], end_idx - start_idx);
    sch_xpath_set_source_index (xpar, end_idx + 1);
}

static void
sch_xpath_skip_space (xpath_scanner *xpar)
{
    while (sch_xpath_is_white_space (xpar->cur_char))
        sch_xpath_next_char (xpar);
}

static char *
sch_xpath_scan_nc_name (xpath_scanner *xpar)
{
    GRegex *regex;
    GMatchInfo *match_info;
    char *match = NULL;
    char *str;
    char select[] = "[:A-Za-z0-9][-._A-Za-z0-9]*";
    char *dummy;
    bool first = true;

    regex = g_regex_new (select, G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, NULL);
    str = &xpar->xpath_expr[xpar->cur_index];
    g_regex_match (regex, str, 0, &match_info);
    while (g_match_info_matches (match_info))
    {
        dummy = g_match_info_fetch (match_info, 0);
        if (first)
        {
            match = g_strdup (dummy);
            xpar->cur_index += strlen (match) - 1;
            sch_xpath_next_char (xpar);
            first = false;
        }
        g_free (dummy);
        g_match_info_next (match_info, NULL);
    }

    g_match_info_free (match_info);
    g_regex_unref (regex);
    return match;
}

static void
sch_xpath_check_token (xpath_scanner *xpar, int kind)
{
    if (XPATH_KIND_FIRST_STRINGABLE > kind)
    {
        DEBUG ("%s:%u Invalid token - kind %d\n", __func__, __LINE__, kind);
    }
    else if (xpar->kind != kind)
    {
        char *err_str = sch_xpath_raw_value (xpar);

        if (kind == XPATH_KIND_EOF)
        {
            DEBUG ("%s:%u Expected end of the expression, found %s\n", __func__, __LINE__,
                   err_str);
        }
        else
        {
            DEBUG ("%s:%u Expected token %s, found %s\n", __func__, __LINE__,
                   sch_xpath_kind_to_string (kind), err_str);
        }

        g_free (err_str);
    }
}

static void
sch_xpath_next_kind (xpath_scanner *xpar)
{
    xpar->prev_end = xpar->cur_index;
    xpar->prev_kind = xpar->kind;
    sch_xpath_skip_space (xpar);
    xpar->start = xpar->cur_index;

    switch (xpar->cur_char)
    {
    case '\0':
        xpar->kind = XPATH_KIND_EOF;
        return;
    case '(':
        xpar->kind = XPATH_KIND_LPARENS;
        sch_xpath_next_char (xpar);
        break;
    case ')':
        xpar->kind = XPATH_KIND_RPARENS;
        sch_xpath_next_char (xpar);
        break;
    case '[':
        xpar->kind = XPATH_KIND_LBRACKET;
        sch_xpath_next_char (xpar);
        break;
    case ']':
        xpar->kind = XPATH_KIND_RBRACKET;
        sch_xpath_next_char (xpar);
        break;
    case '@':
        xpar->kind = XPATH_KIND_AT;
        sch_xpath_next_char (xpar);
        break;
    case ',':
        xpar->kind = XPATH_KIND_COMMA;
        sch_xpath_next_char (xpar);
        break;
    case '$':
        xpar->kind = XPATH_KIND_DOLLAR;
        sch_xpath_next_char (xpar);
        break;
    case '}':
        xpar->kind = XPATH_KIND_RBRACE;
        sch_xpath_next_char (xpar);
        break;
    case '.':
        sch_xpath_next_char (xpar);
        if (xpar->cur_char == '.')
        {
            xpar->kind = XPATH_KIND_DOTDOT;
            sch_xpath_next_char (xpar);
        }
        else if (sch_xpath_is_ascii_digit (xpar->cur_char))
        {
            sch_xpath_set_source_index (xpar, xpar->start);
            xpar->kind = XPATH_KIND_NUMBER;
            sch_xpath_scan_number (xpar);
        }
        else
        {
            xpar->kind = XPATH_KIND_DOT;
        }

        break;
    case ':':
        sch_xpath_next_char (xpar);
        if (xpar->cur_char == ':')
        {
            xpar->kind = XPATH_KIND_COLONCOLON;
            sch_xpath_next_char (xpar);
        }
        else
        {
            xpar->kind = XPATH_KIND_UNKNOWN;
        }

        break;
    case '*':
        xpar->kind = XPATH_KIND_STAR;
        sch_xpath_next_char (xpar);
        sch_xpath_check_operator (xpar, true);
        break;
    case '/':
        sch_xpath_next_char (xpar);
        if (xpar->cur_char == '/')
        {
            xpar->kind = XPATH_KIND_SLASHSLASH;
            sch_xpath_next_char (xpar);
        }
        else
        {
            xpar->kind = XPATH_KIND_SLASH;
        }
        break;
    case '|':
        xpar->kind = XPATH_KIND_UNION;
        sch_xpath_next_char (xpar);
        break;
    case '+':
        xpar->kind = XPATH_KIND_PLUS;
        sch_xpath_next_char (xpar);
        break;
    case '-':
        xpar->kind = XPATH_KIND_MINUS;
        sch_xpath_next_char (xpar);
        break;
    case '=':
        xpar->kind = XPATH_KIND_EQUAL;
        sch_xpath_next_char (xpar);
        break;
    case '!':
        sch_xpath_next_char (xpar);
        if (xpar->cur_char == '=')
        {
            xpar->kind = XPATH_KIND_NOT_EQUAL;
            sch_xpath_next_char (xpar);
        }
        else
        {
            xpar->kind = XPATH_KIND_UNKNOWN;
        }

        break;
    case '<':
        sch_xpath_next_char (xpar);
        if (xpar->cur_char == '=')
        {
            xpar->kind = XPATH_KIND_LESS_EQUAL;
            sch_xpath_next_char (xpar);
        }
        else
        {
            xpar->kind = XPATH_KIND_LESS_THAN;
        }
        break;
    case '>':
        sch_xpath_next_char (xpar);
        if (xpar->cur_char == '=')
        {
            xpar->kind = XPATH_KIND_GREATER_EQUAL;
            sch_xpath_next_char (xpar);
        }
        else
        {
            xpar->kind = XPATH_KIND_GREATER_THAN;
        }
        break;
    case '"':
    case '\'':
        xpar->kind = XPATH_KIND_STRING;
        sch_xpath_scan_string (xpar);
        break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        xpar->kind = XPATH_KIND_NUMBER;
        sch_xpath_scan_number (xpar);
        break;
    default:
        xpar->name = sch_xpath_scan_nc_name (xpar);
        if (xpar->name != NULL)
        {
            xpar->kind = XPATH_KIND_NAME;
            xpar->prefix = g_strdup ("");
            xpar->can_be_function = false;
            xpar->axis = XPATH_AXIS_UNKNOWN;
            bool colon_colon = false;
            int save_source_index = xpar->cur_index;

            // "foo:bar" or "foo:*" -- one lexeme (no spaces allowed)
            // "foo::" or "foo ::"  -- two lexemes, reported as one (AxisName)
            // "foo:?" or "foo :?"  -- lexeme "foo" reported
            if (xpar->cur_char == ':')
            {
                sch_xpath_next_char (xpar);
                if (xpar->cur_char == ':')
                { // "foo::" -> OperatorName, AxisName
                    sch_xpath_next_char (xpar);
                    colon_colon = true;
                    sch_xpath_set_source_index (xpar, save_source_index);
                }
                else
                { // "foo:bar", "foo:*" or "foo:?"
                    char *nc_name = sch_xpath_scan_nc_name (xpar);

                    if (nc_name != NULL)
                    {
                        g_free (xpar->prefix);
                        xpar->prefix = xpar->name;
                        xpar->name = nc_name;
                        // Look ahead for '(' to determine whether QName can be a FunctionName
                        save_source_index = xpar->cur_index;
                        sch_xpath_skip_space (xpar);
                        xpar->can_be_function = (xpar->cur_char == '(');
                        sch_xpath_set_source_index (xpar, save_source_index);
                    }
                    else if (xpar->cur_char == '*')
                    {
                        sch_xpath_next_char (xpar);
                        g_free (xpar->prefix);
                        xpar->prefix = xpar->name;
                        xpar->name = g_strdup ("*");
                    }
                    else
                    {
                        // "foo:?" -> OperatorName, NameTest
                        // Return "foo" and leave ":" to be reported later as an unknown lexeme
                        sch_xpath_set_source_index (xpar, save_source_index);
                    }
                }
            }
            else
            {
                sch_xpath_skip_space (xpar);
                if (xpar->cur_char == ':')
                { // "foo ::" or "foo :?"
                    sch_xpath_next_char (xpar);
                    if (xpar->cur_char == ':')
                    {
                        sch_xpath_next_char (xpar);
                        colon_colon = true;
                    }
                    sch_xpath_set_source_index (xpar, save_source_index);
                }
                else
                {
                    xpar->can_be_function = (xpar->cur_char == '(');
                }
            }
            if (!sch_xpath_check_operator (xpar, false) && colon_colon)
            {
                xpar->axis = sch_xpath_check_axis (xpar);
            }
        }
        else
        {
            xpar->kind = XPATH_KIND_UNKNOWN;
            sch_xpath_next_char (xpar);
        }
        break;
    }
}

static void
sch_xpath_pass_token (xpath_scanner *xpar, int kind)
{
    sch_xpath_check_token (xpar, kind);
    sch_xpath_next_kind (xpar);
}

/**************************************************************************************************/
/*  Expressions                                                                                   */
/**************************************************************************************************/

/*
 *   Expr   ::= OrExpr
 *   OrExpr ::= AndExpr ('or' AndExpr)*
 *   AndExpr ::= EqualityExpr ('and' EqualityExpr)*
 *   EqualityExpr ::= RelationalExpr (('=' | '!=') RelationalExpr)*
 *   RelationalExpr ::= AdditiveExpr (('<' | '>' | '<=' | '>=') AdditiveExpr)*
 *   AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
 *   MultiplicativeExpr ::= UnaryExpr (('*' | 'div' | 'mod') UnaryExpr)*
 *   UnaryExpr ::= ('-')* UnionExpr
 */

static xpath_node *sch_xpath_parse_expr (GList **stack, xpath_scanner *xpar);


static bool
sch_xpath_is_node_type (xpath_scanner *xpar)
{
    return (!xpar->prefix &&
            (g_strcmp0 (sch_xpath_name (xpar), "node") == 0 ||
             g_strcmp0 (sch_xpath_name (xpar), "text") == 0 ||
             g_strcmp0 (sch_xpath_name (xpar), "processing-instruction") == 0 ||
             g_strcmp0 (sch_xpath_name (xpar), "comment") == 0));
}

static bool
sch_xpath_is_primary_expr (xpath_scanner *xpar)
{
    return (xpar->kind == XPATH_KIND_STRING ||
            xpar->kind == XPATH_KIND_NUMBER ||
            xpar->kind == XPATH_KIND_DOLLAR ||
            xpar->kind == XPATH_KIND_LPARENS ||
            (xpar->kind == XPATH_KIND_NAME &&
             sch_xpath_can_be_function (xpar) && !sch_xpath_is_node_type (xpar)));
}

static void
sch_xpath_push_pos_info (GList **stack, int start_char, int end_char)
{
    xpath_pos *pos = g_malloc0 (sizeof (xpath_pos));
    pos->start_char = start_char;
    pos->end_char = end_char;
    *stack = g_list_prepend (*stack, pos);
}

static void
sch_xpath_pop_pos_info (GList **stack)
{
    GList *iter = g_list_first (*stack);

    if (iter)
    {
        g_free (iter->data);
        *stack = g_list_delete_link (*stack, iter);
    }
}

/*
 *   Predicate ::= '[' Expr ']'
 */
static xpath_node *
sch_xpath_parse_predicate (GList **stack, xpath_scanner *xpar)
{
    xpath_node *opnd;

    sch_xpath_pass_token (xpar, XPATH_KIND_LBRACKET);
    opnd = sch_xpath_parse_expr (stack, xpar);
    sch_xpath_pass_token (xpar, XPATH_KIND_RBRACKET);

    return opnd;
}

static xpath_node *
sch_xpath_parse_function_call (GList **stack, xpath_scanner *xpar)
{
    GList *arg_list = NULL;
    char *name = sch_xpath_name (xpar);
    char *prefix = sch_xpath_prefix (xpar);
    int start_char = xpar->start;

    xpar->name = NULL;
    xpar->prefix = NULL;
    sch_xpath_pass_token (xpar, XPATH_KIND_NAME);
    sch_xpath_pass_token (xpar, XPATH_KIND_LPARENS);

    if (xpar->kind != XPATH_KIND_RPARENS)
    {
        while (true)
        {
            arg_list = g_list_append (arg_list, sch_xpath_parse_expr (stack, xpar));
            if (xpar->kind != XPATH_KIND_COMMA)
            {
                sch_xpath_check_token (xpar, XPATH_KIND_RPARENS);
                break;
            }
            sch_xpath_next_kind (xpar); // move off the ','
        }
    }

    sch_xpath_next_kind (xpar); // move off the ')'
    sch_xpath_push_pos_info (stack, start_char, xpar->prev_end);
    xpath_node *result = bld_funcs.function (prefix, name, arg_list);
    sch_xpath_pop_pos_info (stack);
    return result;
}

/*
 *   PrimaryExpr ::= Literal | Number | VariableReference | '(' Expr ')' | FunctionCall
 */
static xpath_node *
sch_xpath_parse_primary_expr (GList **stack, xpath_scanner *xpar)
{
    xpath_node *opnd = NULL;
    char *value;
    int start_char;

    if (!sch_xpath_is_primary_expr (xpar))
    {
        DEBUG ("%s:%u Not primary expression\n", __func__, __LINE__);
        return NULL;
    }

    switch (xpar->kind)
    {
    case XPATH_KIND_STRING:
        opnd = bld_funcs.string (sch_xpath_string_value (xpar));
        g_free (xpar->string_value);
        xpar->string_value = NULL;
        sch_xpath_next_kind (xpar);
        break;
    case XPATH_KIND_NUMBER:
        value = sch_xpath_raw_value (xpar);
        opnd = bld_funcs.number (value);
        g_free (value);
        sch_xpath_next_kind (xpar);
        break;
    case XPATH_KIND_DOLLAR:
        start_char = xpar->start;
        sch_xpath_next_kind (xpar);
        sch_xpath_check_token (xpar, XPATH_KIND_NAME);
        sch_xpath_push_pos_info (stack, start_char,
                                 xpar->start + xpar->cur_index - xpar->start);
        opnd = bld_funcs.variable (sch_xpath_prefix (xpar), sch_xpath_name (xpar));
        xpar->prefix = NULL;
        xpar->name = NULL;
        sch_xpath_pop_pos_info (stack);
        sch_xpath_next_kind (xpar);
        break;
    case XPATH_KIND_LPARENS:
        sch_xpath_next_kind (xpar);
        opnd = sch_xpath_parse_expr (stack, xpar);
        sch_xpath_pass_token (xpar, XPATH_KIND_RPARENS);
        break;
    default:
        if (xpar->kind != XPATH_KIND_NAME || !sch_xpath_can_be_function (xpar) ||
            sch_xpath_is_node_type (xpar))
        {
            DEBUG ("%s:%u sch_xpath_is_primary_expr() returned true, but the kind is not recognized\n",
                   __func__, __LINE__);
        }

        opnd = sch_xpath_parse_function_call (stack, xpar);
        break;
    }
    return opnd;
}

static bool
sch_xpath_is_reverse_axis (xpath_axis axis)
{
    return (axis == XPATH_AXIS_ANCESTOR || axis == XPATH_AXIS_PRECEDING ||
            axis == XPATH_AXIS_ANCESTOR_OR_SELF || axis == XPATH_AXIS_PRECEDING_SIBLING);
}

/*
 *   FilterExpr ::= PrimaryExpr Predicate*
 */
static xpath_node *
sch_xpath_parse_filter_expr (GList **stack, xpath_scanner *xpar)
{
    int start_char = xpar->start;
    xpath_node *opnd = sch_xpath_parse_primary_expr (stack, xpar);
    int end_char = xpar->prev_end;

    while (xpar->kind == XPATH_KIND_LBRACKET)
    {
        sch_xpath_push_pos_info (stack, start_char, end_char);
        sch_xpath_parse_predicate (stack, xpar);
        opnd = bld_funcs.predicate (opnd, sch_xpath_parse_predicate (stack, xpar),
                                    /*reverseStep: */ false);
        sch_xpath_pop_pos_info (stack);
    }
    return opnd;
}

static xpath_node_type
sch_xpath_principal_node_type (xpath_axis axis)
{
    return (axis == XPATH_AXIS_ATTRIBUTE ? XPATH_NODE_TYPE_ATTRIBUTE : axis ==
            XPATH_AXIS_NAMESPACE ? XPATH_NODE_TYPE_NAMESPACE : XPATH_NODE_TYPE_UNKNOWN);
}

static void
sch_xpath_internal_parse_node_test (GList **stack, xpath_scanner *xpar, xpath_axis axis,
                                    xpath_node_type *node_type, char **node_prefix,
                                    char **node_name)
{
    char *value;

    switch (xpar->kind)
    {
    case XPATH_KIND_NAME:
        if (sch_xpath_can_be_function (xpar) && sch_xpath_is_node_type (xpar))
        {
            char *name = sch_xpath_name (xpar);
            *node_prefix = NULL;
            *node_name = NULL;
            if (g_strcmp0 (name, "comment") == 0)
                *node_type = XPATH_NODE_TYPE_COMMENT;
            else if (g_strcmp0 (name, "text") == 0)
                *node_type = XPATH_NODE_TYPE_TEXT;
            else if (g_strcmp0 (name, "node") == 0)
                *node_type = XPATH_NODE_TYPE_ALL;
            else if (g_strcmp0 (name, "processing-instruction") == 0)
                *node_type = XPATH_NODE_TYPE_INSTR;
            else
                DEBUG ("%s:%u node type %s is not recognized\n",
                       __func__, __LINE__, name);

            sch_xpath_next_kind (xpar);
            sch_xpath_pass_token (xpar, XPATH_KIND_LPARENS);

            if (*node_type == XPATH_NODE_TYPE_INSTR)
            {
                if (xpar->kind != XPATH_KIND_RPARENS)
                { // 'processing-instruction' '(' Literal ')'
                    sch_xpath_check_token (xpar, XPATH_KIND_STRING);
                    // It is not needed to set nodePrefix here, but for our current implementation
                    // comparing whole QNames is faster than comparing just local names
                    *node_prefix = g_strdup ("");
                    *node_name = sch_xpath_string_value (xpar);
                    xpar->string_value = NULL;
                    sch_xpath_next_kind (xpar);
                }
            }

            sch_xpath_pass_token (xpar, XPATH_KIND_RPARENS);
        }
        else
        {
            *node_prefix = sch_xpath_prefix (xpar);
            *node_name = sch_xpath_name (xpar);
            xpar->prefix = NULL;
            xpar->name = NULL;
            *node_type = sch_xpath_principal_node_type (axis);
            sch_xpath_next_kind (xpar);
            /* remove the keyword from the prefix and name */
            g_free (xpar->prefix);
            g_free (xpar->name);
            xpar->prefix = NULL;
            xpar->name = NULL;
            if (g_strcmp0 (*node_name, "*") == 0)
            {
                g_free (node_name);
                *node_name = NULL;
            }
        }
        break;
    case XPATH_KIND_STAR:
        *node_prefix = NULL;
        *node_name = NULL;
        *node_type = sch_xpath_principal_node_type (axis);
        sch_xpath_next_kind (xpar);
        break;
    default:
        *node_prefix = NULL;
        *node_name = NULL;
        *node_type = XPATH_NODE_TYPE_UNKNOWN;
        value = sch_xpath_raw_value (xpar);
        DEBUG ("%s:%u Expected a node test, found %s\n", __func__, __LINE__, value);
        g_free (value);
    }
}

/*
 *   NodeTest ::= NameTest | ('comment' | 'text' | 'node') '(' ')' | 'processing-instruction' '('  Literal? ')'
 *   NameTest ::= '*' | NCName ':' '*' | QName
 */
static xpath_node *
sch_xpath_parse_node_test (GList **stack, xpath_scanner *xpar, xpath_axis axis)
{
    xpath_node_type node_type = XPATH_NODE_TYPE_UNKNOWN;
    char *node_prefix;
    xpath_node *result;
    char *node_name;
    int start_char = xpar->start;

    sch_xpath_internal_parse_node_test (stack, xpar, axis, &node_type, &node_prefix,
                                        &node_name);

    sch_xpath_push_pos_info (stack, start_char, xpar->prev_end);
    result = bld_funcs.axis (axis, node_type, node_prefix, node_name);
    sch_xpath_pop_pos_info (stack);
    return result;
}

/*
 *   Step ::= '.' | '..' | (AxisName '::' | '@')? NodeTest Predicate*
 */
static xpath_node *
sch_xpath_parse_step (GList **stack, xpath_scanner *xpar)
{
    xpath_node *opnd;

    if (XPATH_KIND_DOT == xpar->kind)
    { // '.'
        sch_xpath_next_kind (xpar);
        opnd = bld_funcs.axis (XPATH_AXIS_SELF, XPATH_NODE_TYPE_ALL, NULL, NULL);
        if (XPATH_KIND_LBRACKET == xpar->kind)
        {
            DEBUG ("%s:%u Abbreviated step '.' cannot be followed by a predicate\n",
                   __func__, __LINE__);
            sch_xpath_free_node (opnd);
            opnd = NULL;
        }
    }
    else if (XPATH_KIND_DOTDOT == xpar->kind)
    { // '..'
        sch_xpath_next_kind (xpar);
        opnd = bld_funcs.axis (XPATH_AXIS_PARENT, XPATH_NODE_TYPE_ALL, NULL, NULL);
        if (XPATH_KIND_LBRACKET == xpar->kind)
        {
            DEBUG ("%s:%u Abbreviated step '..' cannot be followed by a predicate\n",
                   __func__, __LINE__);
            sch_xpath_free_node (opnd);
            opnd = NULL;
        }
    }
    else
    { // (AxisName '::' | '@')? NodeTest Predicate*
        xpath_axis axis = XPATH_AXIS_UNKNOWN;
        switch (xpar->kind)
        {
        case XPATH_KIND_AXIS:  // AxisName '::'
            axis = xpar->axis;
            sch_xpath_next_kind (xpar);
            sch_xpath_next_kind (xpar);
            break;
        case XPATH_KIND_AT:    // '@'
            axis = XPATH_AXIS_ATTRIBUTE;
            sch_xpath_next_kind (xpar);
            break;
        case XPATH_KIND_NAME:
        case XPATH_KIND_STAR:
            // NodeTest must start with Name or '*'
            axis = XPATH_AXIS_CHILD;
            break;
        default:
            DEBUG ("%s:%u Unexpected token %d in the expression\n", __func__, __LINE__,
                   xpar->kind);
            return NULL;
        }

        opnd = sch_xpath_parse_node_test (stack, xpar, axis);
        while (XPATH_KIND_LBRACKET == xpar->kind)
        {
            opnd = bld_funcs.predicate (opnd, sch_xpath_parse_predicate (stack, xpar),
                                        sch_xpath_is_reverse_axis (axis));
        }
    }

    return opnd;
}

static bool
sch_xpath_is_step (int kind)
{
    // Note node_test is also name
    return (kind == XPATH_KIND_DOT || kind == XPATH_KIND_DOTDOT || kind == XPATH_KIND_AT ||
            kind == XPATH_KIND_AXIS || kind == XPATH_KIND_STAR || kind == XPATH_KIND_NAME);
}

/*
 *   RelativeLocationPath ::= Step (('/' | '//') Step)*
 */
static xpath_node *
sch_xpath_parse_relative_location_path (GList **stack, xpath_scanner *xpar)
{
    xpath_node *opnd;

    if (!sch_xpath_is_step (xpar->kind))
        return NULL;

    opnd = sch_xpath_parse_step (stack, xpar);
    if (xpar->kind == XPATH_KIND_SLASH)
    {
        sch_xpath_next_kind (xpar);
        opnd = bld_funcs.join_step (opnd, sch_xpath_parse_relative_location_path (stack, xpar));
    }
    else if (xpar->kind == XPATH_KIND_SLASHSLASH)
    {
        xpath_node *a_opnd;
        xpath_node *s_opnd;

        sch_xpath_next_kind (xpar);
        a_opnd = bld_funcs.axis (XPATH_AXIS_DESCENDANT_OR_SELF,
                                 XPATH_NODE_TYPE_ALL, NULL, NULL);
        s_opnd =  bld_funcs.join_step (a_opnd, sch_xpath_parse_relative_location_path (stack, xpar));
        opnd = bld_funcs.join_step (opnd, s_opnd);
    }
    return opnd;
}

/*
 *   LocationPath ::= RelativeLocationPath | '/' RelativeLocationPath? | '//' RelativeLocationPath
 */
static xpath_node *
sch_xpath_parse_location_path (GList **stack, xpath_scanner *xpar)
{
    if (xpar->kind == XPATH_KIND_SLASH)
    {
        sch_xpath_next_kind (xpar);
        xpath_node *opnd = bld_funcs.axis (XPATH_AXIS_ROOT, XPATH_NODE_TYPE_ALL, NULL, NULL);

        if (sch_xpath_is_step (xpar->kind))
            opnd = bld_funcs.join_step (opnd,
                                        sch_xpath_parse_relative_location_path (stack, xpar));

        return opnd;
    }
    else if (xpar->kind == XPATH_KIND_SLASHSLASH)
    {
        xpath_node *opnd = bld_funcs.axis (XPATH_AXIS_ROOT, XPATH_NODE_TYPE_ALL, NULL, NULL);
        xpath_node *a_opnd;
        xpath_node *s_opnd;

        a_opnd = bld_funcs.axis (XPATH_AXIS_DESCENDANT_OR_SELF,
                                 XPATH_NODE_TYPE_ALL, NULL, NULL);
        s_opnd = sch_xpath_parse_relative_location_path (stack, xpar);
        sch_xpath_next_kind (xpar);
        opnd =  bld_funcs.join_step (opnd, bld_funcs.join_step (a_opnd, s_opnd));
        return opnd;
    }
    else
    {
        return sch_xpath_parse_relative_location_path (stack, xpar);
    }
}

/*
 *   PathExpr ::= LocationPath | FilterExpr (('/' | '//') RelativeLocationPath )?
 */
static xpath_node *
sch_xpath_parse_path_expr (GList **stack, xpath_scanner *xpar)
{
    // Here we distinguish FilterExpr from LocationPath - the former starts with PrimaryExpr
    if (sch_xpath_is_primary_expr (xpar))
    {
        int start_char = xpar->start;
        xpath_node *opnd = sch_xpath_parse_filter_expr (stack, xpar);
        int end_char = xpar->prev_end;

        if (xpar->kind == XPATH_KIND_SLASH)
        {
            sch_xpath_next_kind (xpar);
            sch_xpath_push_pos_info (stack, start_char, end_char);
            opnd = bld_funcs.join_step (opnd, sch_xpath_parse_relative_location_path (stack, xpar));
            sch_xpath_pop_pos_info (stack);
        }
        else if (xpar->kind == XPATH_KIND_SLASHSLASH)
        {
            xpath_node *a_opnd;
            xpath_node *s_opnd;

            sch_xpath_next_kind (xpar);
            sch_xpath_push_pos_info (stack, start_char, end_char);

            a_opnd = bld_funcs.axis(XPATH_AXIS_DESCENDANT_OR_SELF,
                                    XPATH_NODE_TYPE_ALL, NULL, NULL);
            s_opnd = sch_xpath_parse_relative_location_path (stack, xpar);
            opnd = bld_funcs.join_step (opnd, bld_funcs.join_step (a_opnd, s_opnd));
            sch_xpath_pop_pos_info (stack);
        }
        return opnd;
    }
    else
    {
        return sch_xpath_parse_location_path (stack, xpar);
    }
}

/*
 *   UnionExpr ::= PathExpr ('|' PathExpr)*
 */
static xpath_node *
sch_xpath_parse_union_expr (GList **stack, xpath_scanner *xpar)
{
    int start_char = xpar->start;
    xpath_node *opnd1 = sch_xpath_parse_path_expr (stack, xpar);

    if (xpar->kind == XPATH_KIND_UNION)
    {
        sch_xpath_push_pos_info (stack, start_char, xpar->prev_end);
        opnd1 = bld_funcs.operator (XPATH_OPERATOR_UNION, sch_xpath_allocate_node (), opnd1);
        sch_xpath_pop_pos_info (stack);

        while (xpar->kind == XPATH_KIND_UNION)
        {
            sch_xpath_next_kind (xpar);
            start_char = xpar->start;
            xpath_node *opnd2 = sch_xpath_parse_path_expr (stack, xpar);
            sch_xpath_push_pos_info (stack, start_char, xpar->prev_end);
            opnd1 = bld_funcs.operator (XPATH_OPERATOR_UNION, opnd1, opnd2);
            sch_xpath_pop_pos_info (stack);
        }
    }
    return opnd1;
}

static xpath_node *
sch_xpath_parse_sub_expr (GList **stack, xpath_scanner *xpar, int caller_prec)
{
    // private Node ParseSubExpr(int callerPrec) {
    xpath_operator op;
    xpath_node *opnd;

    // Check for unary operators
    if (xpar->kind == XPATH_KIND_MINUS)
    {
        op = XPATH_OPERATOR_MINUS;
        int op_prec = xpath_operator_precedence[op];
        sch_xpath_next_kind (xpar);
        opnd = bld_funcs.operator (op,
                                   sch_xpath_parse_sub_expr (stack, xpar, op_prec),
                                   sch_xpath_allocate_node ());
    }
    else
    {
        opnd = sch_xpath_parse_union_expr (stack, xpar);
    }

    // Process binary operators
    while (true)
    {
        op = (xpar->kind <=
              XPATH_KIND_LAST_OPERATOR) ? (xpath_operator) xpar->kind :
            XPATH_OPERATOR_UNKNOWN;
        int op_prec = xpath_operator_precedence[op];

        if (op_prec <= caller_prec)
            return opnd;

        // Operator's precedence is greater than the one of our caller, so process it here
        sch_xpath_next_kind (xpar);
        opnd = bld_funcs.operator (op, opnd, sch_xpath_parse_sub_expr (stack, xpar, op_prec));
    }
}

static xpath_node *
sch_xpath_parse_expr (GList **stack, xpath_scanner *xpar)
{
    return sch_xpath_parse_sub_expr (stack, xpar, /*callerPrec: */ 0);
}

static void
sch_show_result (xpath_node *xnode, int depth)
{
    if (!xnode)
        return;

    DEBUG ("node %p depth %d\n", xnode, depth);
    DEBUG ("  op %d op_prec %d reverse_step %d arg_list %p\n", xnode->op,
           xnode->op_prec, xnode->reverse_step, xnode->arg_list);
    DEBUG ("  type %d node_type %s string_value %s number %s, prefix %s name %s axis %s\n",
           xnode->type, xnode->node_type, xnode->string_value, xnode->number,
           xnode->prefix, xnode->name, xnode->axis);
    DEBUG ("  left %p right %p\n", xnode->left, xnode->right);

    if (xnode->arg_list)
    {
        GList *iter;
        xpath_node *lnode;

        for (iter = g_list_first (xnode->arg_list); iter; iter = g_list_next (iter))
        {
            lnode = (xpath_node *) iter->data;
            sch_show_result (lnode, depth + 1);
        }
    }

    if (xnode->left)
        sch_show_result (xnode->left, depth + 1);

    if (xnode->right)
        sch_show_result (xnode->right, depth + 1);
}

xpath_node *
sch_xpath_parser (char *expr)
{
    GList *stack = NULL;
    xpath_node *result;
    xpath_scanner *xpar = g_malloc0 (sizeof (xpath_scanner));

    xpar->xpath_expr = expr;
    xpar->xpath_expr_len = strlen (expr);
    sch_xpath_set_source_index (xpar, 0);
    sch_xpath_next_kind (xpar);
    bld_funcs.start_build ();
    result = sch_xpath_parse_expr (&stack, xpar);
    sch_xpath_check_token (xpar, XPATH_KIND_EOF);
    sch_xpath_free_scanner (xpar);

    result = bld_funcs.end_build (result);
    if (result && (xpath_debug || xpath_verbose))
        sch_show_result (result, 0);

    if (stack)
        DEBUG ("%s:%u xpath_push and xpath_pop calls have been unbalanced\n",
               __func__, __LINE__);
    return result;
}

void
sch_xpath_free_xnode_tree (xpath_node *xnode)
{
    GList *iter;
    xpath_node *cnode;

    if (!xnode)
        return;

    for (iter = g_list_first (xnode->arg_list); iter; iter = g_list_next (iter))
    {
        cnode = (xpath_node *) iter->data;
        sch_xpath_free_xnode_tree (cnode);
    }

    if (xnode->left)
        sch_xpath_free_xnode_tree (xnode->left);

    if (xnode->right)
        sch_xpath_free_xnode_tree (xnode->right);

    sch_xpath_free_node (xnode);
}

char *
sch_xpath_node_type_string (int nt)
{
    if (nt >= 0 && nt < num_nt_strings)
        return g_strdup (xpath_node_type_strings[nt]);

    return NULL;
}

int
sch_xpath_axis_to_type (int axis)
{
    if (axis >= 0 && axis < num_axis_types)
        return xpath_axis_types[axis];

    return XPATH_TYPE_UNKNOWN;
}

int
sch_xpath_op_to_type (int op)
{
    if (op >= 0 && op < num_oper_types)
        return xpath_oper_types[op];

    return XPATH_TYPE_UNKNOWN;
}

void
sch_xpath_build_register (xpath_funcs *funcs, bool debug, bool verbose)
{
    bld_funcs = *funcs;
    xpath_debug = debug;
    xpath_verbose = verbose;
}
