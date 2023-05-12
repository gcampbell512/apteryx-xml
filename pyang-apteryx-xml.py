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
            optparse.make_option("--generate-prefix",
                                 action="store_true",
                                 dest="prefix_default",
                                 default=False,
                                 help="Add a model prefix to the generated XML file."),
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
        self.multiple_modules = True
        fmts['apteryx-xml'] = self

    def setup_fmt(self, ctx):
        ctx.implicit_errors = False

    def emit(self, ctx, modules, fd):
        path = []
        for (epos, etag, eargs) in ctx.errors:
            if error.is_error(error.err_level(etag)):
                raise error.EmitError(
                    "apteryx-xml plugin %s contains errors" % epos.top.arg)
        self.node_handler = {
            "container": self.container,
            "leaf": self.leaf,
            "choice": self.container,
            "case": self.container,
            "list": self.list,
            "leaf-list": self.leaf_list,
        }
        self.prefix_default = ctx.opts.prefix_default
        self.enum_name = ctx.opts.enum_name
        self.ns_uri = {}
        self.model = {}
        self.org = {}
        self.prefix = {}
        self.revision = {}
        for yam in modules:
            self.model[yam] = yam.arg
            ns = yam.search_one('namespace')
            if ns is not None:
                self.ns_uri[yam] = ns.arg
            org = yam.search_one('organization')
            if org is not None:
                self.org[yam] = org.arg
            if (self.prefix_default):
                pref = yam.search_one('prefix')
                if pref is not None:
                    self.prefix[yam] = pref.arg
            rev = yam.search_one('revision')
            if rev is not None:
                self.revision[yam] = rev.arg

        root = etree.Element("MODULE")
        if (yam in self.prefix and yam in self.ns_uri):
            root.set("xmlns", self.ns_uri[yam])
            root.set("xmlns:" + self.prefix[yam], self.ns_uri[yam])
        else:
            root.set("xmlns", "https://github.com/alliedtelesis/apteryx")
        if (yam in self.model):
            root.set("model", self.model[yam])
        if (yam in self.org):
            root.set("organization", self.org[yam])
        if (yam in self.revision):
            root.set("version", self.revision[yam])
        root.set("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance")
        root.set("xsi:schemaLocation", "https://github.com/alliedtelesis/apteryx-xml https://github.com/alliedtelesis/apteryx-xml/releases/download/v1.2/apteryx.xsd")
        for yam in modules:
            self.process_children(yam, root, None, path)
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

        res = etree.SubElement(parent, "NODE")
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
            res = etree.SubElement(res, "NODE")
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
                value = etree.SubElement(res, "VALUE")
                value.attrib = OrderedDict()
                value.attrib["name"] = "true"
                value.attrib["value"] = "true"
                value = etree.SubElement(res, "VALUE")
                value.attrib = OrderedDict()
                value.attrib["name"] = "false"
                value.attrib["value"] = "false"
            if ntype.arg == "enumeration":
                count = 0
                for enum in ntype.substmts:
                    value = etree.SubElement(res, "VALUE")
                    value.attrib = OrderedDict()
                    value.attrib["name"] = enum.arg
                    val = enum.search_one('value')
                    if val is not None:
                        value.attrib["value"] = val.arg
                        try:
                            val_int = int(val.arg)
                        except:
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

        return res, module, path
