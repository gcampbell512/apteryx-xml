<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet version="1.0"
	xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
	xmlns="urn:ietf:params:xml:ns:yang:yin:1"
	xmlns:apteryx="https://github.com/alliedtelesis/apteryx"
	exclude-result-prefixes="apteryx">
	<xsl:output method="xml" indent="yes"/>
	<xsl:strip-space elements="*"/>
	<xsl:param name="prefix" select="'apteryx'"/>

	<xsl:template name="description">
		<xsl:if test="@help">
			<description>
				<text><xsl:value-of select="@help"/></text>
			</description>
		</xsl:if>
	</xsl:template>

	<xsl:template name="config">
		<xsl:if test="@mode='r' or @mode='h'">
			<config value="false"/>
		</xsl:if>
	</xsl:template>

	<xsl:template name="pattern">
		<xsl:if test="@pattern">
			<pattern value="{@pattern}"/>
		</xsl:if>
	</xsl:template>

	<xsl:template name="default">
		<xsl:if test="@default">
			<default value="{@default}"/>
		</xsl:if>
	</xsl:template>

	<xsl:template match="apteryx:VALUE">
		<enum name="{@name}">
			<xsl:if test="number(@value)=@value">
				<value value="{@value}"/>
			</xsl:if>
			<xsl:call-template name="description" />
		</enum>
	</xsl:template>

	<xsl:template name="enumeration">
		<xsl:if test="@default">
			<xsl:variable name="default" select="@default" />
			<default value="{./*[@value=$default]/@name}"/>
		</xsl:if>
		<type name="enumeration">
			<xsl:apply-templates select="./*" />
		</type>
	</xsl:template>

	<xsl:template name="int32">
		<xsl:call-template name="default" />
		<type name="int32">
			<xsl:call-template name="pattern" />
		</type>
	</xsl:template>

	<xsl:template name="string">
		<xsl:call-template name="default" />
		<type name="string">
			<xsl:call-template name="pattern" />
		</type>
	</xsl:template>

	<xsl:template name="leaf">
		<leaf name="{@name}">
			<xsl:call-template name="description" />
			<xsl:call-template name="config" />
			<xsl:choose>
				<xsl:when test="./*[local-name()='VALUE']">
					<xsl:call-template name="enumeration" />
				</xsl:when>
				<xsl:when test="number(@default)=@default">
					<xsl:call-template name="int32" />
				</xsl:when>
				<xsl:otherwise>
					<xsl:call-template name="string" />
				</xsl:otherwise>
			</xsl:choose>
		</leaf>
	</xsl:template>

	<xsl:template name="leaf-list">
		<leaf-list name="{@name}">
			<type name="string" />
			<xsl:call-template name="description" />
			<xsl:if test="./*/@mode='r' or ./*/@mode='h'">
				<config value="false"/>
			</xsl:if>
		</leaf-list>
	</xsl:template>

	<xsl:template name="list">
		<list name="{@name}">
			<key value="{./*[1]/*[1]/@name}"/>
			<xsl:call-template name="description" />
			<xsl:apply-templates select="./*[1]/*" />
		</list>
	</xsl:template>

	<xsl:template name="container">
		<container name="{@name}">
			<xsl:call-template name="description" />
			<xsl:apply-templates select="./*" />
		</container>
	</xsl:template>
	
	<xsl:template match="apteryx:NODE">
		<xsl:choose>
			<xsl:when test="./*[local-name()='VALUE']">
				<xsl:call-template name="leaf" />
			</xsl:when>
			<xsl:when test="not(*)">
				<xsl:call-template name="leaf" />
			</xsl:when>
			<xsl:when test="./*[@name='*'] and not(./*/*)">
				<xsl:call-template name="leaf-list" />
			</xsl:when>
			<xsl:when test="./*[@name='*']">
				<xsl:call-template name="list" />
			</xsl:when>
			<xsl:otherwise>
				<xsl:call-template name="container" />
			</xsl:otherwise>
		</xsl:choose>
	</xsl:template>

	<xsl:template match="apteryx:MODULE">
		<module name="{$prefix}"
			xmlns="urn:ietf:params:xml:ns:yang:yin:1">
			<namespace uri="https://github.com/alliedtelesis/apteryx"/>
			<prefix value="{$prefix}"/>
			<xsl:apply-templates />
		</module>
	</xsl:template>

	<xsl:template match="text()|@*">
	</xsl:template>

</xsl:stylesheet>
