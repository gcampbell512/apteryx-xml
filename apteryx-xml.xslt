<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
	<xsl:output omit-xml-declaration="yes" method="xml" encoding="UTF-8" indent="yes"/>
	<xsl:strip-space elements="*"/>

	<xsl:template match="*[name()='rpc']">
		<!-- Ignore RPC -->
	</xsl:template>

	<xsl:template match="*[name()='grouping']">
		<!-- Do not process these at the top level -->
	</xsl:template>

	<xsl:template match="*[name()='module']">
		<MODULE xmlns:apteryx="https://github.com/alliedtelesis/apteryx" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
		xsi:schemaLocation="https://github.com/alliedtelesis/apteryx
		https://github.com/alliedtelesis/apteryx-xml/releases/download/v1.2/apteryx.xsd">
		<xsl:apply-templates select="node()|@*"/>
		</MODULE>
	</xsl:template>

	<xsl:template match="*[name()='augment' and @target-node = '/if:interfaces/if:interface']">
		<NODE name="interfaces" help="Interface List">
			<NODE name="*" help="Interface Name">
				<xsl:apply-templates select="node()|@*"/>
			</NODE>
		</NODE>
	</xsl:template>

	<xsl:template match="*[name()='augment' and @target-node = '/if:interfaces-state/if:interface']">
		<NODE name="interfaces-state" help="Interface List">
			<NODE name="*" help="Interface Name">
				<xsl:apply-templates select="node()|@*"/>
			</NODE>
		</NODE>
	</xsl:template>

	<xsl:template match="*[name()='container' or name()='leaf' or name()='list' or name()='leaf-list' or name()='choice' or name()='case']">
		<NODE>
		<xsl:attribute name="name"><xsl:value-of select="@name"/></xsl:attribute>
		<xsl:if test="name() = 'leaf'">
			<xsl:choose>
				<xsl:when test="preceding::*[name() = 'config']/@value = 'false'">
					<xsl:attribute name="mode">r</xsl:attribute>
				</xsl:when>
				<xsl:otherwise>
					<xsl:attribute name="mode">rw</xsl:attribute>
				</xsl:otherwise>
			</xsl:choose>
		</xsl:if>
		<xsl:if test="child::*[name() = 'default']">
			<xsl:attribute name="default"><xsl:value-of select="child::*[name() = 'default']/@value"/></xsl:attribute>
		</xsl:if>
		<xsl:if test="child::*[name() = 'description']">
			<xsl:attribute name="help"><xsl:value-of select="normalize-space(child::*[name() = 'description']/.)"/></xsl:attribute>
		</xsl:if>
		<xsl:if test="name() = 'list'">
			<NODE name="*">
			<xsl:attribute name="help">
				<xsl:if test="child::*/@value != ''">
					<xsl:value-of select="concat('The ', @name, ' entry with key ', child::*/@value, '.')"/>
				</xsl:if>
				<xsl:if test="child::*/@value = ''">
					<xsl:value-of select="concat('The ', @name, ' entry', '.')"/>
				</xsl:if>
			</xsl:attribute>
			<xsl:apply-templates select="node()|@*"/>
			</NODE>
		</xsl:if>
		<xsl:if test="name() = 'leaf-list'">
			<NODE name="*">
				<xsl:if test="child::*[name() = 'description']">
					<xsl:attribute name="help"><xsl:value-of select="normalize-space(child::*[name() = 'description']/.)"/></xsl:attribute>
				</xsl:if>
			</NODE>
		</xsl:if>
		<xsl:if test="name() != 'list'">
			<xsl:apply-templates select="node()|@*"/>
		</xsl:if>
		</NODE>
	</xsl:template>

	<xsl:template match="*[name()='uses']">
		<xsl:if test="not(contains(@name, ':'))">
			<xsl:variable name="grouping" select="@name" />
			<xsl:for-each select="preceding::*[name() = 'grouping' and @name = $grouping]">
				<xsl:if test="child::*[name() = 'description']">
					<xsl:attribute name="help"><xsl:value-of select="normalize-space(child::*[name() = 'description']/.)"/></xsl:attribute>
				</xsl:if>
				<xsl:apply-templates select="node()|@*"/>
			</xsl:for-each>
		</xsl:if>
		<xsl:if test="contains(@name, ':')">
			<xsl:variable name="grouping" select="substring-after(@name,':')"/>
			<xsl:variable name="prefix" select="substring-before(@name ,':')"/>
			<xsl:variable name="model" select="preceding::*[name() = 'import' and child::*/@value = $prefix]/@module"/>
			<xsl:variable name="file" select="concat($model,'.yin')"/>
			<xsl:for-each select="document($file)/*[name() = 'module']/*[name() = 'grouping' and @name = $grouping]">
				<xsl:if test="child::*[name() = 'description']">
				<VALUE>
					<xsl:attribute name="help"><xsl:value-of select="normalize-space(child::*[name() = 'description']/.)"/></xsl:attribute>
				</VALUE>
				</xsl:if>
				<xsl:apply-templates select="node()|@*"/>
			</xsl:for-each>
		</xsl:if>
	</xsl:template>

	<xsl:template match="*[name(parent::*) = 'leaf' and name()='type' and @name='enumeration']">
		<xsl:for-each select="child::*">
			<xsl:if test="name() = 'enum'">
				<VALUE>
				<xsl:attribute name="name"><xsl:value-of select="@name"/></xsl:attribute>
				<xsl:attribute name="value"><xsl:value-of select="@name"/></xsl:attribute>
				<xsl:if test="name(child::*) = 'description'">
					<xsl:attribute name="help"><xsl:value-of select="normalize-space(.)"/></xsl:attribute>
				</xsl:if>
				</VALUE>
			</xsl:if>
		</xsl:for-each>
	</xsl:template>

	<xsl:template match="*[name()='type' and preceding::*[name() = 'typedef']/@name = @name]">
		<xsl:variable name="type" select="@name" />
		<xsl:for-each select="preceding::*[name() = 'typedef' and @name = $type]/*[@name='enumeration']/child::*">
			<xsl:if test="name() = 'enum'">
				<VALUE>
				<xsl:attribute name="name"><xsl:value-of select="@name"/></xsl:attribute>
				<xsl:attribute name="value"><xsl:value-of select="@name"/></xsl:attribute>
				<xsl:if test="name(child::*) = 'description'">
					<xsl:attribute name="help"><xsl:value-of select="normalize-space(.)"/></xsl:attribute>
				</xsl:if>
				</VALUE>
			</xsl:if>
		</xsl:for-each>
	</xsl:template>

	<xsl:template match="*[name()='type' and @name='boolean']">
		<VALUE name='true' value='true' />
		<VALUE name='false' value='false' />
	</xsl:template>

	<xsl:template match="*[name()='type' and @name = 'empty']">
		<VALUE name="true" value="true" />
	</xsl:template>

	<xsl:template match="node()|@*">
		<xsl:apply-templates select="node()|@*"/>
	</xsl:template>

	<xsl:template match="text()|@*">
	</xsl:template>

</xsl:stylesheet>
