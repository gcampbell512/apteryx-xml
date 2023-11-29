# Apteryx-XML
An Apteryx database schema defined in XML

## Requires
```
apteryx glib-2.0 libxml2 jansson lua cunit pyang xsltproc
```

## Building
```
make
make test
make install
```

## Schema definition
```xml
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"
  targetNamespace="https://github.com/alliedtelesis/apteryx"
  xmlns="https://github.com/alliedtelesis/apteryx"
  elementFormDefault="qualified">
  <xs:element name="VALUE">
    <xs:complexType>
      <xs:attribute name="name" type="xs:string" use="required" />
      <xs:attribute name="value" type="xs:string" use="required" />
      <xs:attribute name="help" type="xs:string" use="optional" />
    </xs:complexType>
  </xs:element>
  <xs:element name="PROVIDE">
  </xs:element>
  <xs:element name="INDEX">
  </xs:element>
  <xs:element name="WATCH">
  </xs:element>
  <xs:element name="NODE">
    <xs:complexType>
      <xs:sequence>
        <xs:element ref="NODE" minOccurs="0" maxOccurs="unbounded" />
        <xs:element ref="VALUE" minOccurs="0" maxOccurs="unbounded" />
        <xs:element ref="WATCH" minOccurs="0" maxOccurs="1" />
        <xs:element ref="PROVIDE" minOccurs="0" maxOccurs="1" />
        <xs:element ref="INDEX" minOccurs="0" maxOccurs="1" />
      </xs:sequence>
      <xs:attribute name="name" type="xs:string" use="required" />
      <xs:attribute name="default" type="xs:string" use="optional" />
      <xs:attribute name="pattern" type="xs:string" use="optional" />
      <xs:attribute name="help" type="xs:string" use="optional" />
      <xs:attribute name="mode" use="optional">
        <xs:simpleType>
          <xs:restriction base="xs:string">
            <xs:pattern value="(p|x|hx|[rhwc]{1,4})"/>
          </xs:restriction>
        </xs:simpleType>
      </xs:attribute>
    </xs:complexType>
  </xs:element>
  <xs:element name="SCRIPT">
  </xs:element>
  <xs:element name="MODULE">
    <xs:complexType>
      <xs:sequence>
        <xs:element ref="SCRIPT" minOccurs="0" maxOccurs="unbounded"/>
        <xs:element ref="NODE" minOccurs="0" maxOccurs="unbounded"/>
      </xs:sequence>
    </xs:complexType>
  </xs:element>
  </xs:schema>
```

### Example
```xml
<?xml version="1.0" encoding="UTF-8"?>
<MODULE xmlns="https://github.com/alliedtelesis/apteryx"
    xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
    xsi:schemaLocation="https://github.com/alliedtelesis/apteryx
    https://github.com/alliedtelesis/apteryx/releases/download/v2.10/apteryx.xsd">
    <NODE name="test" help="this is a test node">
        <NODE name="debug" mode="rw" default="0" help="Debug configuration" pattern="^(0|1)$">
            <VALUE name="disable" value="0" help="Debugging is disabled" />
            <VALUE name="enable" value="1" help="Debugging is enabled" />
        </NODE>
        <NODE name="list" help="this is a list of stuff">
            <NODE name="*" help="the list item">
                <NODE name="name" mode="rw" help="this is the list key"/>
                <NODE name="type" mode="rw" default="1" help="this is the list type">
                    <VALUE name="big" value="1"/>
                    <VALUE name="little" value="2"/>
                </NODE>
                <NODE name="sub-list" help="this is a list of stuff attached to a list">
                    <NODE name="*" help="the sublist item">
                        <NODE name="i-d" mode="rw" help="this is the sublist key"/>
                    </NODE>
                </NODE>
            </NODE>
        </NODE>
        <NODE name="trivial-list" help="this is a simple list of stuff">
            <NODE name="*" help="the list item" />
        </NODE>
    </NODE>
</MODULE>
```

## Lua library

### Set/Get
```lua
api = require('apteryx-xml').api('/PATH/TO/SCHEMA/')
api.test.debug = 'enable'
assert(api.test.debug == 'enable')
api.test.debug = nil
assert(api.test.debug == 'disable')
```
### Lists
```lua
api = require('apteryx-xml').api('/PATH/TO/SCHEMA/')
api.test.list('cat-nip').sub_list('dog').i_d = '1'
assert(api.test.list('cat-nip').sub_list('dog').i_d == '1')
api.test.list('cat-nip').sub_list('dog').i_d = nil
assert(api.test.list('cat-nip').sub_list('dog').i_d == nil)
```
### Search
```lua
api = require('apteryx-xml').api('/PATH/TO/SCHEMA/')
api.test.list('cat-nip').sub_list('dog').i_d = '1'
api.test.list('cat-nip').sub_list('cat').i_d = '2'
api.test.list('cat-nip').sub_list('mouse').i_d = '3'
api.test.list('cat_nip').sub_list('bat').i_d = '4'
api.test.list('cat_nip').sub_list('frog').i_d = '5'
api.test.list('cat_nip').sub_list('horse').i_d = '6'
cats1 = api.test.list('cat-nip').sub_list()
assert(#cats1 == 3)
cats2 = api.test.list('cat_nip').sub_list()
assert(#cats2 == 3)
api.test.list('cat-nip').sub_list('dog').i_d = nil
api.test.list('cat-nip').sub_list('cat').i_d = nil
api.test.list('cat-nip').sub_list('mouse').i_d = nil
api.test.list('cat_nip').sub_list('bat').i_d = nil
api.test.list('cat_nip').sub_list('frog').i_d = nil
api.test.list('cat_nip').sub_list('horse').i_d = nil
```

## Conversion between other formats

### Generate paths in C header file format

```shell
./xml2c <module>.xml
```

### Convert between YANG and Apteryx-XML

* YANG enumerations assume an implcicit pattern, so patterns on Apteryx-XML enumerations are discarded
* YANG leaf-lists are subtly different to Apteryx-XML simple lists in that there is no name/value pair
* YANG has no concept of visibility, so mode=h becomes mode=r
* Apteryx-XML does not support range (all checking is via regex patterns)

*From YANG to Apteryx-XML*

```shell
pyang --plugindir . -f apteryx-xml <module>.yang
```

*From YANG to C header file format*

```shell
pyang --plugindir . -f cpaths <module>.yang
```

*From Apteryx-XML to YANG*

```shell
xsltproc --stringparam prefix <module> xml2yin.xsl <module>.xml | pyang -f yang
```

## MAP files
The concept behind *.map files is that they allow the user to specify where data for a particular model
is stored in the apteryx database. The files are simple text files with two columns separated be a space.
The first column specifies a namespace URI and the second column specifies the top level node name for the model
in the apteryx database. For example, a map file with the following line:-

http://openconfig.net/yang/system /oc-sys:system

would specify that all openconfig system model data should be stored in the apteryx database under the node /oc-sys:system
Comment lines start with the # character.

The *.map should be saved in the same directory as the input XML files .

## XLAT files
The concept behind the *.xlat files is to allow one model to be translated to another model. This is useful if multiple YANG
models exist for the same type of data. In this case a back-end super YANG model is created which is a combination of
the contributing models, and requests to the individual models are translated into a request of the super model and responses are
translated back into the individual requesting models. In the Apteryx data store information only exists for the super model. The
translation is controlled by a translating configuration file named {name}.xlat. The *.xlat files are LUA files that contain LUA code
to translated data between one model an another. The translation information is returned when an *.xlat file is loaded. An example file
can be found in models/xlat_test.xlat
