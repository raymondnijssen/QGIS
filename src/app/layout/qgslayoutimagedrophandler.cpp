/***************************************************************************
    qgslayoutimagedrophandler.cpp
    ------------------------------
    begin                : November 2019
    copyright            : (C) 2019 by nyall Dawson
    email                : nyall dot dawson at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgslayoutimagedrophandler.h"
#include "qgslayoutdesignerinterface.h"
#include "qgslayout.h"
#include "qgslayoutview.h"
#include "qgslayoutitempicture.h"
#include <QImageReader>

QgsLayoutImageDropHandler::QgsLayoutImageDropHandler( QObject *parent )
  : QgsLayoutCustomDropHandler( parent )
{

}

bool QgsLayoutImageDropHandler::handleFileDrop( QgsLayoutDesignerInterface *iface, QPointF point, const QString &file )
{
  QFileInfo fi( file );

  bool matched = false;
  const QList<QByteArray> formats = QImageReader::supportedImageFormats();
  for ( const QByteArray &format : formats )
  {
    if ( fi.suffix().compare( format, Qt::CaseInsensitive ) == 0 )
    {
      matched = true;
      break;
    }
  }
  if ( !matched )

    return false;

  if ( !iface->layout() )
    return false;

  std::unique_ptr< QgsLayoutItemPicture > item = qgis::make_unique< QgsLayoutItemPicture >( iface->layout() );

  QgsLayoutPoint layoutPoint = iface->layout()->convertFromLayoutUnits( point, iface->layout()->units() );
  item->attemptMove( layoutPoint );

  item->setPicturePath( file );

  // force a resize to the image's actual size
  item->setResizeMode( QgsLayoutItemPicture::FrameToImageSize );
  // and then move back to standard freeform image sizing
  item->setResizeMode( QgsLayoutItemPicture::Zoom );

  // and auto select new item for convenience
  QList< QgsLayoutItem * > newSelection;
  newSelection << item.get();
  iface->layout()->addLayoutItem( item.release() );
  iface->layout()->deselectAll();
  iface->selectItems( newSelection );

  return true;
}
