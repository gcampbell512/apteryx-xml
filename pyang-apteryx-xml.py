"""

Output paths in Apteryx XML file format

"""
import io
import sys
import optparse
import xml.etree.ElementTree as etree
from collections import OrderedDict

from pyang import error, plugin


def pyang_plugin_init():
    plugin.register_plugin(ApteryxXMLPlugin())


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
        ntype = node.search_one("type")
        if ntype is not None and ntype.arg in module.i_typedefs:
            typedef = module.i_typedefs[ntype.arg].copy()
            typedef.arg = node.arg
            ndescr = node.search_one('description')
            if ndescr is not None:
                tdescr = typedef.search_one('description')
                if tdescr is None:
                    typedef.substmts.append(ndescr)
                else:
                    tdescr.arg = ndescr.arg
            typedef.i_config = node.i_config
            if node.i_default is not None:
                typedef.i_default = node.i_default
                typedef.i_default_str = node.i_default_str
            typedef.keyword = node.keyword
            nel, newm, path = self.sample_element(typedef, elem, module, path)
        else:
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
        if node.keyword == 'leaf':
            if node.i_config:
                res.attrib["mode"] = "rw"
            else:
                res.attrib["mode"] = "r"
            if node.i_default is not None:
                res.attrib["default"] = node.i_default_str
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
        if ntype is not None:
            if ntype.arg == "string":
                npatt = ntype.search_one("pattern")
                if npatt is not None:
                    res.attrib["pattern"] = npatt.arg
            if ntype.arg == "boolean":
                value = etree.SubElement(res, "{" + ns.arg + "}VALUE")
                value.attrib = OrderedDict()
                value.attrib["name"] = "true"
                value.attrib["value"] = "true"
                value = etree.SubElement(res, "{" + ns.arg + "}VALUE")
                value.attrib = OrderedDict()
                value.attrib["name"] = "false"
                value.attrib["value"] = "false"
            if ntype.arg == "enumeration":
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
            if ntype.arg in ["int8", "int16", "int32", "int64", "uint8", "uint16"]:
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
            if ntype.arg in ["uint32", "uint64"]:
                # These values are actually encoded as strings
                range = ntype.search_one("range")
                if range is not None:
                    # TODO convert range into a regex pattern
                    res.attrib["range"] = range.arg
                elif ntype.arg == "int64":
                    # range="-9223372036854775808..9223372036854775807"
                    res.attrib["pattern"] = "(-([0-9]{1,18}|[1-8][0-9]{18}|9([01][0-9]{17}|2([01][0-9]{16}|2([0-2][0-9]{15}|3([0-2][0-9]{14}|3([0-6][0-9]{13}|7([01][0-9]{12}|20([0-2][0-9]{10}|3([0-5][0-9]{9}|6([0-7][0-9]{8}|8([0-4][0-9]{7}|5([0-3][0-9]{6}|4([0-6][0-9]{5}|7([0-6][0-9]{4}|7([0-4][0-9]{3}|5([0-7][0-9]{2}|80[0-8]))))))))))))))))|([0-9]{1,18}|[1-8][0-9]{18}|9([01][0-9]{17}|2([01][0-9]{16}|2([0-2][0-9]{15}|3([0-2][0-9]{14}|3([0-6][0-9]{13}|7([01][0-9]{12}|20([0-2][0-9]{10}|3([0-5][0-9]{9}|6([0-7][0-9]{8}|8([0-4][0-9]{7}|5([0-3][0-9]{6}|4([0-6][0-9]{5}|7([0-6][0-9]{4}|7([0-4][0-9]{3}|5([0-7][0-9]{2}|80[0-7])))))))))))))))))""
                elif ntype.arg == "uint64":
                    # range="0..18446744073709551615"
                    res.attrib["pattern"] = "([0-9]{1,19}|1([0-7][0-9]{18}|8([0-3][0-9]{17}|4([0-3][0-9]{16}|4([0-5][0-9]{15}|6([0-6][0-9]{14}|7([0-3][0-9]{13}|4([0-3][0-9]{12}|40([0-6][0-9]{10}|7([0-2][0-9]{9}|3([0-6][0-9]{8}|70([0-8][0-9]{6}|9([0-4][0-9]{5}|5([0-4][0-9]{4}|5(0[0-9]{3}|1([0-5][0-9]{2}|6(0[0-9]|1[0-5])))))))))))))))))"
        return res, module, path
