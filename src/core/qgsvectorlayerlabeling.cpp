/***************************************************************************
    qgsvectorlayerlabeling.cpp
    ---------------------
    begin                : September 2015
    copyright            : (C) 2015 by Martin Dobias
    email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "qgsvectorlayerlabeling.h"

#include "qgspallabeling.h"
#include "qgsrulebasedlabeling.h"
#include "qgsvectorlayer.h"

QgsAbstractVectorLayerLabeling::~QgsAbstractVectorLayerLabeling()
{
}

void QgsAbstractVectorLayerLabeling::toSld( QDomNode& parent, const QgsVectorLayer* layer ) const
{
  writeSld( parent, layer );
}

QgsAbstractVectorLayerLabeling* QgsAbstractVectorLayerLabeling::create( const QDomElement& element )
{
  if ( element.attribute( "type" ) == "rule-based" )
  {
    return QgsRuleBasedLabeling::create( element );
  }
  else
  {
    // default
    return new QgsVectorLayerSimpleLabeling;
  }
}

void QgsAbstractVectorLayerLabeling::writeSld( QDomNode& parent, const QgsVectorLayer* layer ) const
{
  Q_UNUSED( layer )
  Q_UNUSED( parent )
}

QgsVectorLayerLabelProvider* QgsVectorLayerSimpleLabeling::provider( QgsVectorLayer* layer ) const
{
  if ( layer->customProperty( "labeling" ).toString() == QLatin1String( "pal" ) && layer->labelsEnabled() )
    return new QgsVectorLayerLabelProvider( layer, QString(), false );

  return nullptr;
}

QString QgsVectorLayerSimpleLabeling::type() const
{
  return "simple";
}

QDomElement QgsVectorLayerSimpleLabeling::save( QDomDocument& doc ) const
{
  // all configuration is kept in layer custom properties (for compatibility)
  QDomElement elem = doc.createElement( "labeling" );
  elem.setAttribute( "type", "simple" );
  return elem;
}

QgsPalLayerSettings QgsVectorLayerSimpleLabeling::settings( const QgsVectorLayer* layer, const QString& providerId ) const
{
  if ( providerId.isEmpty() )
    return QgsPalLayerSettings::fromLayer( layer );
  else
    return QgsPalLayerSettings();
}

void QgsVectorLayerSimpleLabeling::writeSld( QDomNode& featureTypeStyleElement, const QgsVectorLayer *layer ) const
{

  QgsPalLayerSettings labelingSettings = settings( layer );

  if ( labelingSettings.drawLabels ) {

      QDomDocument doc = featureTypeStyleElement.ownerDocument();

      QDomElement ruleElement = doc.createElement( "se:Rule" );
      featureTypeStyleElement.appendChild( ruleElement );

      QDomElement textSymbolizerElement = doc.createElement( "se:TextSymbolizer" );
      //labeling.setAttribute( "font", labelingSettings.textFont.family() );
      ruleElement.appendChild( textSymbolizerElement );

      // label

      QDomElement labelElement = doc.createElement( "se:Label" );
      textSymbolizerElement.appendChild( labelElement );

      QDomElement propertyNameElement = doc.createElement( "ogc:PropertyName" );
      propertyNameElement.appendChild( doc.createTextNode( labelingSettings.fieldName ) );
      labelElement.appendChild(propertyNameElement);

      // font

      QDomElement fontElement = doc.createElement( "se:Font" );
      textSymbolizerElement.appendChild( fontElement );

      addCssParameter( fontElement, "font-family", labelingSettings.textFont.family() );
      addCssParameter( fontElement, "font-size", QString( labelingSettings.textFont.pixelSize() ) );


      // labelplacement

      // fill

      QDomElement fillElement = doc.createElement( "se:Fill" );
      textSymbolizerElement.appendChild( fillElement );

      addCssParameter( fillElement, "fill", QString( labelingSettings.textColor.name() ) );


  }





}

void QgsAbstractVectorLayerLabeling::addCssParameter( QDomElement& parent, const QString& attributeName, const QString& attributeValue) const
{

    QDomElement cssParameterElement = parent.ownerDocument().createElement( "se:CssParameter" );
    cssParameterElement.setAttribute( "name", attributeName );
    cssParameterElement.appendChild( parent.ownerDocument().createTextNode( attributeValue ) );
    parent.appendChild(cssParameterElement);
}
