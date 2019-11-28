/*
 *    libpal - Automated Placement of Labels Library
 *
 *    Copyright (C) 2008 Maxence Laurent, MIS-TIC, HEIG-VD
 *                             University of Applied Sciences, Western Switzerland
 *                             http://www.hes-so.ch
 *
 *    Contact:
 *        maxence.laurent <at> heig-vd <dot> ch
 *     or
 *        eric.taillard <at> heig-vd <dot> ch
 *
 * This file is part of libpal.
 *
 * libpal is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libpal is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libpal.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qgsgeometry.h"
#include "pal.h"
#include "layer.h"
#include "palexception.h"
#include "palstat.h"
#include "rtree.hpp"
#include "costcalculator.h"
#include "feature.h"
#include "geomfunction.h"
#include "labelposition.h"
#include "problem.h"
#include "pointset.h"
#include "internalexception.h"
#include "util.h"
#include <cfloat>
#include <list>

using namespace pal;

Pal::Pal()
{
  // do not init and exit GEOS - we do it inside QGIS
  //initGEOS( geosNotice, geosError );
}

void Pal::removeLayer( Layer *layer )
{
  if ( !layer )
    return;

  mMutex.lock();
  if ( QgsAbstractLabelProvider *key = mLayers.key( layer, nullptr ) )
  {
    mLayers.remove( key );
    delete layer;
  }
  mMutex.unlock();
}

Pal::~Pal()
{

  mMutex.lock();

  qDeleteAll( mLayers );
  mLayers.clear();
  mMutex.unlock();

  // do not init and exit GEOS - we do it inside QGIS
  //finishGEOS();
}

Layer *Pal::addLayer( QgsAbstractLabelProvider *provider, const QString &layerName, QgsPalLayerSettings::Placement arrangement, double defaultPriority, bool active, bool toLabel, bool displayAll )
{
  mMutex.lock();

  Q_ASSERT( !mLayers.contains( provider ) );

  Layer *layer = new Layer( provider, layerName, arrangement, defaultPriority, active, toLabel, this, displayAll );
  mLayers.insert( provider, layer );
  mMutex.unlock();

  return layer;
}

struct FeatCallBackCtx
{
  Layer *layer = nullptr;

  std::list< std::unique_ptr< Feats > > *features = nullptr;

  RTree<FeaturePart *, double, 2, double> *obstacleIndex = nullptr;
  RTree<LabelPosition *, double, 2, double> *candidateIndex = nullptr;
  QList<LabelPosition *> *positionsWithNoCandidates = nullptr;
  const GEOSPreparedGeometry *mapBoundary = nullptr;
  Pal *pal = nullptr;
};



/*
 * Callback function
 *
 * Extract a specific shape from indexes
 */
bool extractFeatCallback( FeaturePart *featurePart, void *ctx )
{
  double amin[2], amax[2];
  FeatCallBackCtx *context = reinterpret_cast< FeatCallBackCtx * >( ctx );

  // Holes of the feature are obstacles
  for ( int i = 0; i < featurePart->getNumSelfObstacles(); i++ )
  {
    featurePart->getSelfObstacle( i )->getBoundingBox( amin, amax );
    context->obstacleIndex->Insert( amin, amax, featurePart->getSelfObstacle( i ) );

    if ( !featurePart->getSelfObstacle( i )->getHoleOf() )
    {
      //ERROR: SHOULD HAVE A PARENT!!!!!
    }
  }

  // generate candidates for the feature part
  std::vector< std::unique_ptr< LabelPosition > > candidates = featurePart->createCandidates();

  // purge candidates that are outside the bbox
  candidates.erase( std::remove_if( candidates.begin(), candidates.end(), [&context]( std::unique_ptr< LabelPosition > &candidate )
  {
    if ( context->pal->showPartialLabels() )
      return !candidate->intersects( context->mapBoundary );
    else
      return !candidate->within( context->mapBoundary );
  } ), candidates.end() );

  if ( !candidates.empty() )
  {
    for ( std::unique_ptr< LabelPosition > &candidate : candidates )
    {
      candidate->insertIntoIndex( context->candidateIndex );
    }

    std::sort( candidates.begin(), candidates.end(), CostCalculator::candidateSortGrow );

    // valid features are added to fFeats
    std::unique_ptr< Feats > ft = qgis::make_unique< Feats >();
    ft->feature = featurePart;
    ft->shape = nullptr;
    ft->candidates = std::move( candidates );
    ft->priority = featurePart->calculatePriority();
    context->features->emplace_back( std::move( ft ) );
  }
  else
  {
    // features with no candidates are recorded in the unlabeled feature list
    std::unique_ptr< LabelPosition > unplacedPosition = featurePart->createCandidatePointOnSurface( featurePart );
    if ( unplacedPosition )
      context->positionsWithNoCandidates->append( unplacedPosition.release() );
  }

  return true;
}

struct ObstacleCallBackCtx
{
  RTree<FeaturePart *, double, 2, double> *obstacleIndex = nullptr;
  int obstacleCount = 0;
};

/*
 * Callback function
 *
 * Extract obstacles from indexes
 */
bool extractObstaclesCallback( FeaturePart *ft_ptr, void *ctx )
{
  double amin[2], amax[2];
  ObstacleCallBackCtx *context = reinterpret_cast< ObstacleCallBackCtx * >( ctx );

  // insert into obstacles
  ft_ptr->getBoundingBox( amin, amax );
  context->obstacleIndex->Insert( amin, amax, ft_ptr );
  context->obstacleCount++;
  return true;
}

struct FilterContext
{
  RTree<LabelPosition *, double, 2, double> *candidateIndex = nullptr;
  Pal *pal = nullptr;
};

bool filteringCallback( FeaturePart *featurePart, void *ctx )
{

  RTree<LabelPosition *, double, 2, double> *cdtsIndex = ( reinterpret_cast< FilterContext * >( ctx ) )->candidateIndex;
  Pal *pal = ( reinterpret_cast< FilterContext * >( ctx ) )->pal;

  if ( pal->isCanceled() )
    return false; // do not continue searching

  double amin[2], amax[2];
  featurePart->getBoundingBox( amin, amax );

  LabelPosition::PruneCtx pruneContext;
  pruneContext.obstacle = featurePart;
  pruneContext.pal = pal;
  cdtsIndex->Search( amin, amax, LabelPosition::pruneCallback, static_cast< void * >( &pruneContext ) );

  return true;
}

std::unique_ptr<Problem> Pal::extract( const QgsRectangle &extent, const QgsGeometry &mapBoundary )
{
  // to store obstacles
  RTree<FeaturePart *, double, 2, double> obstacles;
  std::unique_ptr< Problem > prob = qgis::make_unique< Problem >();

  double bbx[4];
  double bby[4];

  double amin[2];
  double amax[2];

  std::size_t max_p = 0;

  bbx[0] = bbx[3] = amin[0] = prob->mMapExtentBounds[0] = extent.xMinimum();
  bby[0] = bby[1] = amin[1] = prob->mMapExtentBounds[1] = extent.yMinimum();
  bbx[1] = bbx[2] = amax[0] = prob->mMapExtentBounds[2] = extent.xMaximum();
  bby[2] = bby[3] = amax[1] = prob->mMapExtentBounds[3] = extent.yMaximum();

  prob->pal = this;

  std::list< std::unique_ptr< Feats > > features;

  FeatCallBackCtx context;

  // prepare map boundary
  geos::unique_ptr mapBoundaryGeos( QgsGeos::asGeos( mapBoundary ) );
  geos::prepared_unique_ptr mapBoundaryPrepared( GEOSPrepare_r( QgsGeos::getGEOSHandler(), mapBoundaryGeos.get() ) );

  context.features = &features;
  context.obstacleIndex = &obstacles;
  context.candidateIndex = prob->mCandidatesIndex;
  context.positionsWithNoCandidates = prob->positionsWithNoCandidates();
  context.mapBoundary = mapBoundaryPrepared.get();
  context.pal = this;

  ObstacleCallBackCtx obstacleContext;
  obstacleContext.obstacleIndex = &obstacles;
  obstacleContext.obstacleCount = 0;

  // first step : extract features from layers

  std::size_t previousFeatureCount = 0;
  int previousObstacleCount = 0;

  QStringList layersWithFeaturesInBBox;

  mMutex.lock();
  for ( Layer *layer : qgis::as_const( mLayers ) )
  {
    if ( !layer )
    {
      // invalid layer name
      continue;
    }

    // only select those who are active
    if ( !layer->active() )
      continue;

    // check for connected features with the same label text and join them
    if ( layer->mergeConnectedLines() )
      layer->joinConnectedFeatures();

    layer->chopFeaturesAtRepeatDistance();

    layer->mMutex.lock();

    // find features within bounding box and generate candidates list
    context.layer = layer;
    layer->mFeatureIndex->Search( amin, amax, extractFeatCallback, static_cast< void * >( &context ) );
    // find obstacles within bounding box
    layer->mObstacleIndex->Search( amin, amax, extractObstaclesCallback, static_cast< void * >( &obstacleContext ) );

    layer->mMutex.unlock();

    if ( context.features->size() - previousFeatureCount > 0 || obstacleContext.obstacleCount > previousObstacleCount )
    {
      layersWithFeaturesInBBox << layer->name();
    }
    previousFeatureCount = context.features->size();
    previousObstacleCount = obstacleContext.obstacleCount;
  }
  mMutex.unlock();

  prob->mLayerCount = layersWithFeaturesInBBox.size();
  prob->labelledLayersName = layersWithFeaturesInBBox;

  prob->mFeatureCount = features.size();
  prob->mTotalCandidates = 0;
  prob->mFeatNbLp = new int [prob->mFeatureCount];
  prob->mFeatStartId = new int [prob->mFeatureCount];
  prob->mInactiveCost = new double[prob->mFeatureCount];

  if ( !features.empty() )
  {
    // Filtering label positions against obstacles
    amin[0] = amin[1] = std::numeric_limits<double>::lowest();
    amax[0] = amax[1] = std::numeric_limits<double>::max();
    FilterContext filterCtx;
    filterCtx.candidateIndex = prob->mCandidatesIndex;
    filterCtx.pal = this;
    obstacles.Search( amin, amax, filteringCallback, static_cast< void * >( &filterCtx ) );

    if ( isCanceled() )
    {
      return nullptr;
    }

    int idlp = 0;
    for ( std::size_t i = 0; i < prob->mFeatureCount; i++ ) /* foreach feature into prob */
    {
      std::unique_ptr< Feats > feat = std::move( features.front() );
      features.pop_front();

      prob->mFeatStartId[i] = idlp;
      prob->mInactiveCost[i] = std::pow( 2, 10 - 10 * feat->priority );

      switch ( feat->feature->getGeosType() )
      {
        case GEOS_POINT:
          max_p = feat->feature->layer()->maximumPointLabelCandidates();
          break;
        case GEOS_LINESTRING:
          max_p = feat->feature->layer()->maximumLineLabelCandidates();
          break;
        case GEOS_POLYGON:
          max_p = feat->feature->layer()->maximumPolygonLabelCandidates();
          break;
      }

      // sort candidates by cost, skip less interesting ones, calculate polygon costs (if using polygons)
      max_p = CostCalculator::finalizeCandidatesCosts( feat.get(), max_p, &obstacles, bbx, bby );

      // only keep the 'max_p' best candidates
      while ( feat->candidates.size() > max_p )
      {
        // TODO remove from index
        feat->candidates.back()->removeFromIndex( prob->mCandidatesIndex );
        feat->candidates.pop_back();
      }

      // update problem's # candidate
      prob->mFeatNbLp[i] = static_cast< int >( feat->candidates.size() );
      prob->mTotalCandidates += static_cast< int >( feat->candidates.size() );

      // add all candidates into a rtree (to speed up conflicts searching)
      for ( std::size_t j = 0; j < feat->candidates.size(); j++, idlp++ )
      {
        //lp->insertIntoIndex(prob->candidates);
        feat->candidates[ j ]->setProblemIds( static_cast< int >( i ), idlp ); // bugfix #1 (maxence 10/23/2008)
      }
      features.emplace_back( std::move( feat ) );
    }

    int nbOverlaps = 0;

    while ( !features.empty() ) // foreach feature
    {
      if ( isCanceled() )
      {
        return nullptr;
      }

      std::unique_ptr< Feats > feat = std::move( features.front() );
      features.pop_front();

      for ( std::unique_ptr< LabelPosition > &candidate : feat->candidates )
      {
        std::unique_ptr< LabelPosition > lp = std::move( candidate );

        lp->resetNumOverlaps();

        // make sure that candidate's cost is less than 1
        lp->validateCost();

        //prob->feat[idlp] = j;

        lp->getBoundingBox( amin, amax );

        // lookup for overlapping candidate
        prob->mCandidatesIndex->Search( amin, amax, LabelPosition::countOverlapCallback, static_cast< void * >( lp.get() ) );

        nbOverlaps += lp->getNumOverlaps();

        prob->addCandidatePosition( lp.release() );
      }
    }
    nbOverlaps /= 2;
    prob->mAllNblp = prob->mTotalCandidates;
    prob->mNbOverlap = nbOverlaps;
  }

  return prob;
}

void Pal::registerCancellationCallback( Pal::FnIsCanceled fnCanceled, void *context )
{
  fnIsCanceled = fnCanceled;
  fnIsCanceledContext = context;
}

std::unique_ptr<Problem> Pal::extractProblem( const QgsRectangle &extent, const QgsGeometry &mapBoundary )
{
  return extract( extent, mapBoundary );
}

QList<LabelPosition *> Pal::solveProblem( Problem *prob, bool displayAll, QList<LabelPosition *> *unlabeled )
{
  if ( !prob )
    return QList<LabelPosition *>();

  prob->reduce();

  try
  {
    prob->chain_search();
  }
  catch ( InternalException::Empty & )
  {
    return QList<LabelPosition *>();
  }

  return prob->getSolution( displayAll, unlabeled );
}


void Pal::setMaximumNumberOfPointCandidates( int candidates )
{
  if ( candidates > 0 )
    this->mMaxPointCandidates = candidates;
}

void Pal::setMaximumNumberOfLineCandidates( int line_p )
{
  if ( line_p > 0 )
    this->mMaxLineCandidates = line_p;
}

void Pal::setMaximumNumberOfPolygonCandidates( int poly_p )
{
  if ( poly_p > 0 )
    this->mMaxPolyCandidates = poly_p;
}


void Pal::setMinIt( int min_it )
{
  if ( min_it >= 0 )
    mTabuMinIt = min_it;
}

void Pal::setMaxIt( int max_it )
{
  if ( max_it > 0 )
    mTabuMaxIt = max_it;
}

void Pal::setPopmusicR( int r )
{
  if ( r > 0 )
    mPopmusicR = r;
}

void Pal::setEjChainDeg( int degree )
{
  this->mEjChainDeg = degree;
}

void Pal::setTenure( int tenure )
{
  this->mTenure = tenure;
}

void Pal::setCandListSize( double fact )
{
  this->mCandListSize = fact;
}

void Pal::setShowPartialLabels( bool show )
{
  this->mShowPartialLabels = show;
}

int Pal::maximumNumberOfPointCandidates() const
{
  return mMaxPointCandidates;
}

int Pal::maximumNumberOfLineCandidates() const
{
  return mMaxLineCandidates;
}

int Pal::maximumNumberOfPolygonCandidates() const
{
  return mMaxPolyCandidates;
}

QgsLabelingEngineSettings::PlacementEngineVersion Pal::placementVersion() const
{
  return mPlacementVersion;
}

void Pal::setPlacementVersion( QgsLabelingEngineSettings::PlacementEngineVersion placementVersion )
{
  mPlacementVersion = placementVersion;
}

int Pal::getMinIt()
{
  return mTabuMaxIt;
}

int Pal::getMaxIt()
{
  return mTabuMinIt;
}

bool Pal::showPartialLabels() const
{
  return mShowPartialLabels;
}
