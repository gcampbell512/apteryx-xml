"""

Output paths in Apteryx XML file format

"""

# RobertKimble:
# This is my answer to the challenge. It doesn't always find the shortest
# expression, but it works pretty well. Let me know if you find any bugs.
# If the original author wants to blow this away and go back, feel free.
# I'm not trying to rain on anybody's parade.

# There are a couple differences. I don't care in which order the inputs are
# given. They can be longs of any length (within reason). I assume the inputs
# are numbers. The expression I output allows for leading zeros. Not a big
# deal really, but that's the way I designed it.

# I have added a parse tree class to help factor out common prefixes. It's
# somewhat broken, so I have put its use into a try/except block. I'll work
# on fixing it when I have time.
#
# NOTE:
# This code was taken from http://utilitymill.com/edit/Regex_For_Range -
# all code submitted there is done so under GPL

import io
import sys
import os
import optparse
import xml.etree.ElementTree as etree
from collections import OrderedDict

from pyang import error, plugin, statements


def pyang_plugin_init():
    plugin.register_plugin(ApteryxXMLPlugin())


def regex_for_range(start, end, verbose=False):

    import re

    class rfr_tree(object):
        '''Holds set of patterns in a tree in order to factor out common prefixes.'''

        def __init__(self, key, patterns):
            '''Recursively builds the parse tree.'''
            self.node = key
            self.branches = []
            if 0 == len(patterns):
                return
            if 1 == len(patterns):
                pattern = patterns[0]
                if pattern:
                    root = pattern[0]
                    if pattern[1:]:
                        self.branches = [rfr_tree(root, [pattern[1:]])]
                    else:
                        self.branches = [rfr_tree(root, [])]
                return
            # The recursive part:
            roots = []
            for p in patterns:
                if p:
                    h = p[0]
                    if h not in roots:
                        roots.append(h)
                else:
                    self.branches.append(rfr_tree('', []))
            # roots = sorted(set(self.get_head(p) for p in patterns if p))
            for root in roots:
                branch = rfr_tree(root, [p[1:] for p in patterns if p and root == p[0]])
                self.branches.append(branch)

        def collapse(self):
            '''Collapses the parsed tree into a compact regex.'''
            if 0 == len(self.branches):
                return self.node
            if 1 == len(self.branches):
                return self.node + self.branches[0].collapse()
            b = [branch.collapse() for branch in self.branches]
            if '' in b:
                b.remove('')
                if 1 == len(b):
                    b0 = b[0]
                    if 1 == len(b0) or (len(b0) <= 5 and b0.startswith('[') and b0.endswith(']')):
                        return self.node + b0 + '?'
                return self.node + '(' + '|'.join(b) + ')?'
            return self.node + '(' + '|'.join(b) + ')'

        def to_grid(self):
            grid = []
            node = self.node
            if not node:
                node = '.'
            if self.branches:
                for branch in self.branches:
                    for g in branch.to_grid():
                        grid.append([node] + g)
                return grid
            else:
                return [[node]]

        def print_tree(self):
            print('Parse into tree based on regex prefixes:')
            grid = self.to_grid()
            prev_row = ''
            for i, row in enumerate(grid):
                if 0 == i:
                    out_row = row
                else:
                    out_row = list(row)
                    m = -1
                    for j in range(min(len(row), len(prev_row))):
                        if row[j] == prev_row[j]:
                            m = j
                            out_row[j] = ' ' * len(row[j])
                            # spacer[j] = ('|' + spacer[j])[:-1]
                    if 0 <= m:
                        out_row[m] = ('+' + '-' * len(out_row[m]))[:-1]
                    # print ' '.join(spacer)
                print(' {}'.format(' '.join(out_row)))
                # Save a pointer for the next row's use:
                prev_row = row
                # Create a spacer row for the next row:
                # spacer = [' ' * len(x) for x in row]
            print()

    def break_into_ranges_1(start, end):
        '''Breaks the input range into discrete set of equal length ranges.'''
        # Turn the inputs into longs:
        start = int(start)
        end = int(end)
        if len(str(start)) == len(str(end)):
            return [(str(start), str(end))]
        break_point = 10 ** len(str(start)) - 1
        return [(str(start), str(break_point))] + break_into_ranges_1(
            1 + break_point, end)

    def fix_pair(pair):
        '''Prepends 0's as necessary to first member to make it have the same
           length as the second.'''
        start, end = pair
        return (start.rjust(len(end), '0'), end)

    def str_bp(break_point):
        '''Returns break_point and break_point + 1 as equal length strings.'''
        return fix_pair((str(break_point), str(break_point + 1)))

    def break_into_ranges_2(start, end):
        '''Does the grunt work of breaking the range into parts that can
           readily be turned into regex'.'''
        if 1 == len(start):
            return [(start, end)]
        if '0' * len(start) == '0' + start[1:]:
            if '9' * len(end) == '9' + end[1:]:
                return [(start, end)]
            if start[0] < end[0]:
                break_point = int(end[0] + '0' * len(end[1:])) - 1
                bp, bp1 = str_bp(break_point)
                return [(start, bp)] + break_into_ranges_2(bp1, end)
        if '9' * len(end) == '9' + end[1:]:
            if start[0] < end[0]:
                break_digit = str(1 + int(start[0]))
                break_point = int(break_digit + '0' * len(end[1:])) - 1
                bp, bp1 = str_bp(break_point)
                return break_into_ranges_2(start, bp) + [(bp1, end)]
        if start[0] < end[0]:
            break_digit = str(1 + int(start[0]))
            break_point = int(break_digit + '0' * len(end[1:])) - 1
            bp, bp1 = str_bp(break_point)
            return break_into_ranges_2(start, bp) + break_into_ranges_2(bp1, end)
        digit = start[0]
        bir2 = break_into_ranges_2(start[1:], end[1:])
        return [(digit + s, digit + e) for (s, e) in bir2]

    def break_into_ranges(start, end):
        '''Combines break_into_ranges_1 and break_into_ranges_2.'''
        bir = []
        if verbose:
            print('First, break into equal length ranges:')
        bir1 = []
        for s, e in break_into_ranges_1(start, end):
            bir1.append((s, e))
            bir.extend(break_into_ranges_2(s, e))
        if verbose:
            for s, e in bir1:
                print(' {}-{}'.format(s, e))
            print()
            print('Second, break into ranges that yield simple regexes:')
            for s, e in bir:
                print(' {}-{}'.format(s, e))
            print()
        return bir

    # To allow for testing:
    globals()['break_into_ranges'] = break_into_ranges

    def individual_regex(start, end):
        '''Computes one compact regex representing the range indicated.'''
        p = ''
        for i in range(len(start)):
            if start[i] == end[i]:
                p += start[i]
            elif 1 + int(start[i]) == int(end[i]):
                p += '[%s%s]' % (start[i], end[i])
            else:
                p += '[%s-%s]' % (start[i], end[i])
        return shrink(p, len(end))

    def ranges_to_regexes(ranges):
        '''Builds a list of the individual regexes.'''
        return [individual_regex(s, e) for s, e in ranges]

    def range_to_regexes(start, end):
        '''Wrapper function for ranges_to_regexes.'''
        s1, e1 = int(start), int(end)
        if s1 > e1:
            s1, e1 = e1, s1
        s1, e1 = str(s1), str(e1)
        r = ranges_to_regexes(break_into_ranges(s1, e1))
        if verbose:
            print('Turn each range into a regex:')
            for rgx in r:
                print(' {}'.format(rgx))
            print()
        return r

    def collapse_powers_of_10(regexes):
        '''Collapses the powers of 10 into one compact range.'''
        regexes = list(regexes)  # Get a new list
        regexes.append('')  # Append an empty string to make life easier.
        regexes2 = []  # What we're going to output.
        # Used to track where the powers of 10 part starts:
        p10start = -1
        # Used to track where the powers of 10 part ends:
        p10end = -1
        # Whether or not the range we're collapsing starts with 0:
        starts_with_0 = False
        for regex in regexes:
            if '[0-9]' == regex:  # This is the only way the 0 case can happen.
                p10start = 0
                p10end = 0
                starts_with_0 = True
                regex = ''
            elif '[1-9]' == regex:
                p10start = 0
                p10end = 0
                regex = ''
            elif '[1-9][0-9]' == regex:
                if p10start < 0:
                    p10start = 1
                p10end = 1
                regex = ''
            elif regex.startswith('[1-9][0-9]{'):  # Remember, these have been shrunk.
                n = int(regex[len('[1-9][0-9]{'):-1])
                if p10start < 0:
                    p10start = n
                p10end = n
                regex = ''
            elif 0 <= p10start:  # If we get here, we've run out of powers of 10.
                if starts_with_0:
                    newregex = '[0-9]'
                    if 1 <= p10end:
                        newregex += '{1,%d}' % (1 + p10end,)
                else:
                    newregex = '[1-9]'
                    if 1 <= p10end:
                        newregex += '[0-9]'
                    if 0 == p10start and 1 == p10end:
                        newregex += '?'
                    elif p10start == p10end and 1 < p10start:
                        newregex += '{%d}' % (p10start,)
                    elif p10start < p10end:
                        newregex += '{%d,%d}' % (p10start, p10end)
                p10start = -1
                p10end = -1
                regexes2.append(newregex)
            if regex:
                regexes2.append(regex)
        if verbose:
            print('Collapse adjacent powers of 10:')
            for rgx in regexes2:
                print(' {}'.format(rgx))
            print()
        return regexes2

    def tokenize(r):
        '''Tokenizes a regex into a list of tokens.'''
        tokens = []  # Start a list:
        if r:
            reToken = re.compile(r'(\d|\[[^\]]*\])(\?|\{[^}]*\})?')
            token = reToken.match(r).group(0)
            tokens.append(token)
            tokens.extend(tokenize(r[len(token):]))
        return tokens

    def rfr(start, end, verbose=False):
        '''Computes the regex from the ranges supplied by break_into_ranges.'''
        start = str(start)
        end = str(end)
        r = range_to_regexes(start, end)
        c = collapse_powers_of_10(r)
        if 1 == len(c):
            return lead_zeros + c[0]
        s = lead_zeros + r'(' + '|'.join(c) + r')'
        if '|' in s:
            if verbose:
                print('Combining the regexes above yields:')
                print(' {}'.format(s))
                print()
                print('''Next we'll try factoring out common prefixes using a tree:''')
            # Try using rfr_tree to factor out common prefixes.
            try:
                # This is totally cheesy, because the exception shouldn't happen.
                t = [tokenize(rgx) for rgx in c]
                t2 = rfr_tree('', t)
                if verbose:
                    t2.print_tree()
                s2 = lead_zeros + t2.collapse()
                if len(s2) < len(s):
                    # Only use if it's actually shorter.
                    s = s2
                if verbose:
                    print('Turning the parse tree into a regex yields:')
                    print(' {}'.format(s2))
                    print()
                    print('We choose the shorter one as our result.')
                    print()
            except Exception:
                if verbose:
                    print(' Uh-oh -- problem creating parse tree.')
                    print()
                pass
        return s

    def shrink(regex, maxlen):
        '''Looks for cheap ways to shrink the regex.'''
        for i in range(maxlen, 1, -1):
            regex = regex.replace('[0-9]' * i, '[0-9]{%d}' % i)
        return regex

    return rfr(start, end, verbose)


lead_zeros = ""


class RegexForRange():
    def __init__(self, start, end):
        self.start = start
        self.end = end

    def __repr__(self):
        """
        This function is different to the original from utilitymill.com.
        It has been modified to ensure that the resulting regular expressions
        do not match leading zeros or "-0". Also, ensure that the whole thing
        is surrounded in brackets.
        """
        if self.start > self.end:
            raise AssertionError("")

        if self.start > 0:
            return "" + regex_for_range(self.start, self.end, False)
        elif self.start == 0 and self.end > 0:
            return "(0|" + regex_for_range(1, self.end, False) + ")"
        elif self.start == 0 and self.end == 0:
            return "0"
        elif self.start < 0 and self.end == 0:
            return "(0|-" + regex_for_range(1, -self.start, False) + ")"
        elif self.start < 0 and self.end < 0:
            return "(-" + regex_for_range(-self.end, -self.start, False) + ")"
        elif self.start < 0 and self.end > 0:
            return "(-" + regex_for_range(1, -self.start, False) + "|0|" + regex_for_range(1, self.end, False) + ")"
        else:
            raise AssertionError("")


# Patch ElementTree._serialize_xml for ordered attributes
def _serialize_xml(write, elem, encoding, qnames, namespaces):
    tag = elem.tag
    text = elem.text
    if tag is etree.Comment:
        write("<!--%s-->" % etree._encode(text, encoding))
    elif tag is etree.ProcessingInstruction:
        write("<?%s?>" % etree._encode(text, encoding))
    else:
        tag = qnames[tag]
        if tag is None:
            if text:
                write(etree._escape_cdata(text, encoding))
            for e in elem:
                _serialize_xml(write, e, encoding, qnames, None)
        else:
            write("<" + tag)
            items = elem.items()
            if items or namespaces:
                if namespaces:
                    for v, k in sorted(namespaces.items(),
                                       key=lambda x: x[1]):  # sort on prefix
                        if k:
                            k = ":" + k
                        write(" xmlns%s=\"%s\"" % (
                            k.encode(encoding),
                            etree._escape_attrib(v, encoding)
                        ))
                for k, v in items:
                    if isinstance(k, etree.QName):
                        k = k.text
                    if isinstance(v, etree.QName):
                        v = qnames[v.text]
                    else:
                        v = etree._escape_attrib(v, encoding)
                    write(" %s=\"%s\"" % (qnames[k], v))
            if text or len(elem):
                write(">")
                if text:
                    write(etree._escape_cdata(text, encoding))
                for e in elem:
                    _serialize_xml(write, e, encoding, qnames, None)
                write("</" + tag + ">")
            else:
                write("/>")
    if elem.tail:
        write(etree._escape_cdata(elem.tail, encoding))


def _serialize_xml3(write, elem, qnames, namespaces,
                    short_empty_elements, **kwargs):
    tag = elem.tag
    text = elem.text
    if tag is etree.Comment:
        write("<!--%s-->" % text)
    elif tag is etree.ProcessingInstruction:
        write("<?%s?>" % text)
    else:
        tag = qnames[tag]
        if tag is None:
            if text:
                write(etree._escape_cdata(text))
            for e in elem:
                _serialize_xml(write, e, qnames, None,
                               short_empty_elements=short_empty_elements)
        else:
            write("<" + tag)
            items = list(elem.items())
            if items or namespaces:
                if namespaces:
                    for v, k in sorted(namespaces.items(),
                                       key=lambda x: x[1]):  # sort on prefix
                        if k:
                            k = ":" + k
                        write(" xmlns%s=\"%s\"" % (
                            k,
                            etree._escape_attrib(v)
                        ))
                for k, v in items:
                    if isinstance(k, etree.QName):
                        k = k.text
                    if isinstance(v, etree.QName):
                        v = qnames[v.text]
                    else:
                        v = etree._escape_attrib(v)
                    write(" %s=\"%s\"" % (qnames[k], v))
            if text or len(elem) or not short_empty_elements:
                write(">")
                if text:
                    write(etree._escape_cdata(text))
                for e in elem:
                    etree._serialize_xml(write, e, qnames, None,
                                         short_empty_elements=short_empty_elements)
                write("</" + tag + ">")
            else:
                write("/>")
    if elem.tail:
        write(etree._escape_cdata(elem.tail))


if sys.version > "3":
    etree._serialize_xml = _serialize_xml3
else:
    etree._serialize_xml = _serialize_xml


class ApteryxXMLPlugin(plugin.PyangPlugin):

    def add_opts(self, optparser):
        optlist = [
            optparse.make_option("--enum-name",
                                 action="store_true",
                                 dest="enum_name",
                                 default=False,
                                 help="Use the enum name as the value unless specified"),
            ]
        g = optparser.add_option_group(
            "generate-prefix option")
        g.add_options(optlist)

    def add_output_format(self, fmts):
        self.multiple_modules = False
        fmts['apteryx-xml'] = self

    def setup_fmt(self, ctx):
        ctx.implicit_errors = False

    def emit(self, ctx, modules, fd):
        module = modules[0]
        path = []
        for (epos, etag, eargs) in ctx.errors:
            if error.is_error(error.err_level(etag)):
                raise error.EmitError(
                    "apteryx-xml plugin %s contains errors" % epos.top.arg)
        self.node_handler = {
            "container": self.container,
            "leaf": self.leaf,
            "choice": self.choice,
            "case": self.case,
            "list": self.list,
            "leaf-list": self.leaf_list,
            "action": self.rpc,
            "rpc": self.rpc,
            "input": self.rpc,
            "output": self.rpc,
        }
        self.enum_name = ctx.opts.enum_name

        # Create the root node
        root = etree.Element("MODULE")
        root.set("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance")
        root.set("xsi:schemaLocation", "https://github.com/alliedtelesis/apteryx-xml "
                 "https://github.com/alliedtelesis/apteryx-xml/releases/download/v1.2/apteryx.xsd")
        root.set("model", module.arg)
        namespace = module.search_one('namespace')
        if namespace is not None:
            root.set("namespace", namespace.arg)
        prefix = module.search_one('prefix')
        if prefix is not None:
            root.set("prefix", prefix.arg)
        org = module.search_one('organization')
        if org is not None:
            root.set("organization", org.arg)
        rev = module.search_one('revision')
        if rev is not None:
            root.set("version", rev.arg)
        if ctx.opts.features:
            # Ignore the features list if it is empty
            if len(ctx.opts.features[0]) > 0:
                features_string = ','.join(ctx.opts.features)
                root.set("features", features_string)
        if ctx.opts.deviations:
            lst = []
            for x in ctx.opts.deviations:
                lst.append(os.path.basename(os.path.splitext(x)[0]))
            deviations_string = ','.join(lst)
            root.set("deviations", deviations_string)

        # Add any included/imported models
        for m in module.search("include"):
            subm = ctx.get_module(m.arg)
            if subm is not None:
                modules.append(subm)
        for m in module.search("import"):
            subm = ctx.get_module(m.arg)
            if subm is not None:
                modules.append(subm)

        # Register all namespaces
        for m in modules:
            ns = m.search_one('namespace')
            pref = m.search_one('prefix')
            if ns is not None and pref is not None:
                etree.register_namespace(pref.arg, ns.arg)
        if namespace is not None:
            if prefix is not None:
                etree.register_namespace(prefix.arg, namespace.arg)
            # This must be last!
            etree.register_namespace("", namespace.arg)
        else:
            etree.register_namespace("", "https://github.com/alliedtelesis/apteryx")

        # Process all NODEs
        for m in modules:
            self.process_children(m, root, module, path)

        # Dump output
        self.format(root, indent="  ")
        stream = io.BytesIO()
        etree.ElementTree(root).write(stream, 'UTF-8', xml_declaration=True)
        fd.write(stream.getvalue().decode('UTF-8'))

    def format(self, elem, level=0, indent="  "):
        i = "\n" + level * indent
        if len(elem):
            if not elem.text or not elem.text.strip():
                elem.text = i + indent
            if not elem.tail or not elem.tail.strip():
                elem.tail = i
            for elem in elem:
                self.format(elem, level + 1, indent)
            if not elem.tail or not elem.tail.strip():
                elem.tail = i
        else:
            if level and (not elem.tail or not elem.tail.strip()):
                elem.tail = i

    def ignore(self, node, elem, module, path):
        pass

    def process_children(self, node, elem, module, path, omit=[]):
        for ch in node.i_children:
            if ch not in omit:
                self.node_handler.get(ch.keyword, self.ignore)(
                    ch, elem, module, path)

    def rpc(self, node, elem, module, path):
        if (node.keyword == 'input' or node.keyword == 'output') and len(node.substmts) == 0:
            return
        parent = elem
        if node.keyword == 'rpc':
            root = elem
            while root.find('..'):
                root = root.find('..')
            parent = root.find(".//NODE[@name='operations']")
            if parent is None:
                parent = etree.SubElement(root, "NODE")
                parent.attrib = OrderedDict()
                parent.attrib["name"] = "operations"
        nel, newm, path = self.sample_element(node, parent, module, path)
        if path is None:
            return
        self.process_children(node, nel, newm, path)

    def container(self, node, elem, module, path):
        nel, newm, path = self.sample_element(node, elem, module, path)
        if path is None:
            return
        self.process_children(node, nel, newm, path)

    def choice(self, node, elem, module, path):
        self.process_children(node, elem, module, path)

    def case(self, node, elem, module, path):
        self.process_children(node, elem, module, path)

    def leaf(self, node, elem, module, path):
        nel, newm, path = self.sample_element(node, elem, module, path)

    def list(self, node, elem, module, path):
        nel, newm, path = self.sample_element(node, elem, module, path)
        if path is None:
            return
        for kn in node.i_key:
            self.node_handler.get(kn.keyword, self.ignore)(
                kn, nel, newm, path)
        self.process_children(node, nel, newm, path, node.i_key)

    def leaf_list(self, node, elem, module, path):
        nel, newm, path = self.sample_element(node, elem, module, path)

    def node_in_namespace(self, node, ns):
        chns = node.i_module.search_one('namespace')
        if chns is not None and chns == ns:
            return True
        if (hasattr(node, "i_children")):
            for ch in node.i_children:
                if self.node_in_namespace(ch, ns):
                    return True
        return False

    def node_descendant_of(self, node, keyword):
        while node.parent is not None:
            if node.parent.keyword == keyword:
                return True
            node = node.parent
        return False

    def value_attrib_identityref(self, res, ntype):
        if hasattr(ntype, "i_type_spec") and hasattr(ntype.i_type_spec, "idbases"):
            for ch in ntype.i_type_spec.idbases:
                sp_parts = ch.arg.split(':', 2);
                if len(sp_parts) == 1:
                    if ch.i_module is not None and ch.i_module.i_prefix is not None:
                        res.attrib["idref_self"] = ch.i_module.i_prefix
                else:
                    if ch.i_module.i_prefixes is not None:
                        for pref, mname in ch.i_module.i_prefixes.items():
                            if pref == sp_parts[0]:
                                module_name = mname[:][0]
                                subm = ch.i_module.i_ctx.get_module(module_name)
                                if subm is not None:
                                        ns = subm.search_one('namespace')
                                        if ns is not None and subm.i_prefix is not None:
                                            res.attrib["idref_href"] = ns.arg
                                            res.attrib["idref_prefix"] = subm.i_prefix
                                            res.attrib["idref_module"] = module_name
                                            return

    def value_identityref(self, node, res):
        ntype = node.search_one("type")
        if ntype and ntype.i_typedef is not None:
            ntype = ntype.i_typedef.search_one("type")
        if ntype is not None:
            if ntype.arg == "identityref":
                self.value_attrib_identityref(res, ntype)
            if ntype.arg == "union" and hasattr(ntype, "i_type_spec"):
                if (hasattr(ntype.i_type_spec, "types")):
                    base_idref = True
                    for ch in ntype.i_type_spec.types:
                        if ch.arg != "identityref":
                            base_idref = False
                            break
                    if base_idref and hasattr(ch, "i_type_spec"):
                        self.value_attrib_identityref(res, ch)

    def type_to_pattern(self, ntype):
        """
        Return a pattern for a given node type. Return None if
        a pattern is not appropriate.
        """
        patt = None
        rfr = None
        if not isinstance(ntype, statements.Statement):
            return None
        if ntype.arg in ["int8", "int16", "int32", "uint8", "uint16", "uint32"]:
            range = ntype.search_one("range")
            if range is not None:
                limits = range.arg.replace('..', '.').split('.')
                l0 = int(limits[0])
                l1 = int(limits[1])
                if l0 > l1:
                    l1, l0 = l0, l1
                rfr = RegexForRange(l0, l1)
            elif ntype.arg == "int8":
                rfr = RegexForRange(-128, 127)
            elif ntype.arg == "int16":
                rfr = RegexForRange(-32768, 32767)
            elif ntype.arg == "int32":
                rfr = RegexForRange(-2147483648, 2147483647)
            elif ntype.arg == "uint8":
                rfr = RegexForRange(0, 255)
            elif ntype.arg == "uint16":
                rfr = RegexForRange(0, 65535)
            elif ntype.arg == "uint32":
                rfr = RegexForRange(0, 4294967295)
            if rfr is not None:
                patt = f"{rfr}"
            return patt
        return None

    def sample_element(self, node, parent, module, path):
        if path is None:
            return parent, module, None
        elif path == []:
            pass
        else:
            if node.arg == path[0]:
                path = path[1:]
            else:
                return parent, module, None
        # Do not keep this node if it or its children are not in the modules namespace
        if not self.node_in_namespace(node, module.search_one('namespace')):
            return parent, module, None
        ns = node.i_module.search_one('namespace')
        res = etree.SubElement(parent, "{" + ns.arg + "}NODE")
        res.attrib = OrderedDict()
        res.attrib["name"] = node.arg
        if node.keyword == 'rpc' or node.keyword == 'action':
            res.attrib["mode"] = "rwx"
        if node.keyword == 'leaf':
            self.value_identityref(node, res)
            if node.i_config:
                res.attrib["mode"] = "rw"
            elif self.node_descendant_of(node, "input"):
                res.attrib["mode"] = "w"
            elif self.node_descendant_of(node, "output"):
                res.attrib["mode"] = "r"
            else:
                res.attrib["mode"] = "r"
            if node.i_default is not None:
                res.attrib["default"] = node.i_default_str

        niffeature = node.search_one("if-feature")
        if niffeature is not None:
            res.attrib["if-feature"] = niffeature.arg

        # Check for a "when" clause in a "uses" on an augmented container
        if node.keyword == 'container':
            if hasattr(node, 'i_augment'):
                naug = node.i_augment
                for ch in naug.i_children:
                    if hasattr(ch, 'i_uses'):
                        for uses in ch.i_uses:
                            nwhen = uses.search_one("when")
                            if nwhen is not None:
                                res.attrib["when"] = nwhen.arg

        nwhen = node.search_one("when")
        if nwhen is not None:
            res.attrib["when"] = nwhen.arg

        nmust = node.search_one("must")
        if nmust is not None:
            res.attrib["must"] = nmust.arg

        descr = node.search_one('description')
        if descr is not None:
            descr.arg = descr.arg.replace('\r', ' ').replace('\n', ' ')
            res.attrib["help"] = descr.arg

        if node.keyword is not None and (node.keyword == "list" or node.keyword == "leaf-list"):
            res = etree.SubElement(res, "{" + ns.arg + "}NODE")
            res.attrib = OrderedDict()
            res.attrib["name"] = "*"
            key = node.search_one("key")
            if node.keyword == "leaf-list":
                if node.i_config:
                    res.attrib["mode"] = "rw"
                else:
                    res.attrib["mode"] = "r"
            if key is not None:
                res.attrib["help"] = "The " + node.arg + " entry with key " + key.arg
            else:
                res.attrib["help"] = "List of " + node.arg

        ntype = node.search_one("type")
        if ntype and ntype.i_typedef is not None:
            ntype = ntype.i_typedef.search_one("type")
        if ntype is not None:
            if ntype.arg == "string":
                npatt = ntype.search_one("pattern")
                if npatt is not None:
                    res.attrib["pattern"] = npatt.arg
            elif ntype.arg == "boolean":
                value = etree.SubElement(res, "{" + ns.arg + "}VALUE")
                value.attrib = OrderedDict()
                value.attrib["name"] = "true"
                value.attrib["value"] = "true"
                value = etree.SubElement(res, "{" + ns.arg + "}VALUE")
                value.attrib = OrderedDict()
                value.attrib["name"] = "false"
                value.attrib["value"] = "false"
            elif ntype.arg == "enumeration":
                count = 0
                for enum in ntype.substmts:
                    value = etree.SubElement(res, "{" + ns.arg + "}VALUE")
                    value.attrib = OrderedDict()
                    value.attrib["name"] = enum.arg
                    val = enum.search_one('value')
                    if val is not None:
                        value.attrib["value"] = val.arg
                        try:
                            val_int = int(val.arg)
                        except ValueError:
                            val_int = None
                        if val_int is not None:
                            count = val_int
                    else:
                        if self.enum_name:
                            value.attrib["value"] = value.attrib["name"]
                        else:
                            value.attrib["value"] = str(count)
                    count = count + 1
                    descr = enum.search_one('description')
                    if descr is not None:
                        descr.arg = descr.arg.replace('\r', ' ').replace('\n', ' ')
                        value.attrib["help"] = descr.arg
            elif ntype.arg in ["int8", "int16", "int32", "uint8", "uint16", "uint32"]:
                range = ntype.search_one("range")
                if range is not None:
                    res.attrib["range"] = range.arg
                elif ntype.arg == "int8":
                    res.attrib["range"] = "-128..127"
                elif ntype.arg == "int16":
                    res.attrib["range"] = "-32768..32767"
                elif ntype.arg == "int32":
                    res.attrib["range"] = "-2147483648..2147483647"
                elif ntype.arg == "uint8":
                    res.attrib["range"] = "0..255"
                elif ntype.arg == "uint16":
                    res.attrib["range"] = "0..65535"
                elif ntype.arg == "uint32":
                    res.attrib["range"] = "0..4294967295"
            elif ntype.arg in ["int64", "uint64"]:
                # These values are actually encoded as strings
                range = ntype.search_one("range")
                if range is not None:
                    # TODO convert range into a regex pattern
                    res.attrib["range"] = range.arg
                elif ntype.arg == "int64":
                    # range="-9223372036854775808..9223372036854775807"
                    res.attrib["pattern"] = "(-([0-9]{1,18}|[1-8][0-9]{18}|9([01][0-9]{17}|2([01][0-9]{16}|2([0-2][0-9]{15}|3([0-2][0-9]{14}|3([0-6][0-9]{13}|7([01][0-9]{12}|20([0-2][0-9]{10}|3([0-5][0-9]{9}|6([0-7][0-9]{8}|8([0-4][0-9]{7}|5([0-3][0-9]{6}|4([0-6][0-9]{5}|7([0-6][0-9]{4}|7([0-4][0-9]{3}|5([0-7][0-9]{2}|80[0-8]))))))))))))))))|([0-9]{1,18}|[1-8][0-9]{18}|9([01][0-9]{17}|2([01][0-9]{16}|2([0-2][0-9]{15}|3([0-2][0-9]{14}|3([0-6][0-9]{13}|7([01][0-9]{12}|20([0-2][0-9]{10}|3([0-5][0-9]{9}|6([0-7][0-9]{8}|8([0-4][0-9]{7}|5([0-3][0-9]{6}|4([0-6][0-9]{5}|7([0-6][0-9]{4}|7([0-4][0-9]{3}|5([0-7][0-9]{2}|80[0-7])))))))))))))))))"
                elif ntype.arg == "uint64":
                    # range="0..18446744073709551615"
                    res.attrib["pattern"] = "([0-9]{1,19}|1([0-7][0-9]{18}|8([0-3][0-9]{17}|4([0-3][0-9]{16}|4([0-5][0-9]{15}|6([0-6][0-9]{14}|7([0-3][0-9]{13}|4([0-3][0-9]{12}|40([0-6][0-9]{10}|7([0-2][0-9]{9}|3([0-6][0-9]{8}|70([0-8][0-9]{6}|9([0-4][0-9]{5}|5([0-4][0-9]{4}|5(0[0-9]{3}|1([0-5][0-9]{2}|6(0[0-9]|1[0-5])))))))))))))))))"
            elif ntype.arg == 'union':
                uniontypes = ntype.search('type')
                upatt = []
                for uniontype in uniontypes:
                    if uniontype.i_typedef:
                        ut = uniontype.i_typedef.search_one("type")
                        npatt = ut.search_one("pattern")
                        if npatt is not None:
                            upatt.append(f"(^{npatt.arg}$)")
                        else:
                            utpatt = self.type_to_pattern(ut)
                            if utpatt is not None:
                                upatt.append(f"(^{utpatt}$)")

                if len(upatt) > 0:
                    res.attrib["pattern"] = "|".join(upatt)
        return res, module, path
