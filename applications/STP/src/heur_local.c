/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2019 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not visit scip.zib.de.         */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   heur_local.c
 * @brief  Improvement heuristic for Steiner problems
 * @author Daniel Rehfeldt
 *
 * This file implements several local heuristics, including vertex insertion, key-path exchange and key-vertex elimination,
 * ("Fast Local Search for Steiner Trees in Graphs" by Uchoa and Werneck). Other heuristics are for PCSTP and MWCSP.
 *
 * A list of all interface methods can be found in heur_local.h.
 *
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/
//#define SCIP_DEBUG
#include <assert.h>
#include <string.h>
#include "heur_local.h"
#include "heur_tm.h"
#include "probdata_stp.h"
#include "cons_stp.h"


/* @note if heuristic is running in root node timing is changed there to (SCIP_HEURTIMING_DURINGLPLOOP |
 *       SCIP_HEURTIMING_BEFORENODE), see SCIP_DECL_HEURINITSOL callback
 */

#define HEUR_NAME             "local"
#define HEUR_DESC             "improvement heuristic for STP"
#define HEUR_DISPCHAR         '-'
#define HEUR_PRIORITY         100
#define HEUR_FREQ             1
#define HEUR_FREQOFS          0
#define HEUR_MAXDEPTH         -1
#define HEUR_TIMING           (SCIP_HEURTIMING_BEFORENODE | SCIP_HEURTIMING_DURINGLPLOOP | SCIP_HEURTIMING_AFTERLPLOOP | SCIP_HEURTIMING_AFTERNODE)

#define HEUR_USESSUBSCIP      FALSE  /**< does the heuristic use a secondary SCIP instance? */

#define DEFAULT_DURINGROOT    TRUE
#define DEFAULT_MAXFREQLOC    FALSE
#define DEFAULT_MAXNBESTSOLS  30
#define DEFAULT_NBESTSOLS     15
#define DEFAULT_MINNBESTSOLS  10
#define LOCAL_MAXRESTARTS  6

#define GREEDY_MAXRESTARTS  3  /**< Max number of restarts for greedy PC/MW heuristic if improving solution has been found. */
#define GREEDY_EXTENSIONS_MW 6   /**< Number of extensions for greedy MW heuristic. MUST BE HIGHER THAN GREEDY_EXTENSIONS */
#define GREEDY_EXTENSIONS    5  /**< Number of extensions for greedy PC heuristic. */


/*
 * Data structures
 */

/** primal heuristic data */
struct SCIP_HeurData
{
   int                   nfails;             /**< number of fails */
   int                   maxnsols;           /**< maximal number of best solutions to improve */
   int                   nbestsols;          /**< number of best solutions to improve */
   int*                  lastsolindices;     /**< indices of a number of best solutions already tried */
   SCIP_Bool             maxfreq;            /**< should the heuristic be called with maximum frequency? */
   SCIP_Bool             duringroot;         /**< should the heuristic be called during the root node? */
};

/** Voronoi data */
typedef struct Voronoi_data_structures
{
   PATH*                 vnoi_path;           /**< path */
   int*                  vnoi_base;           /**< base*/
   SCIP_Real*            memvdist;           /**< distance */
   int*                  memvbase;           /**< base*/
   int*                  meminedges;         /**< in-edge */
   int*                  vnoi_nodestate;     /**< node state */
   int                   nmems;              /**< number of memorized elements */
   int                   nkpnodes;           /**< number of key path nodes */
} VNOI;


/** Connectivity data */
typedef struct connectivity_data
{
   IDX**                 blists_start;       /**< boundary lists starts (on nodes) */
   IDX**                 lvledges_start;     /**< horizontal edges starts (on nodes) */
   PHNODE**              pheap_boundpaths;   /**< boundary paths (on nodes) */
   int*                  pheap_sizes;        /**< size (on nodes) */
   int*                  boundedges;         /**< boundary edge */
   UF*                   uf;                 /**< union find */
   int                   nboundedges;        /**< number of bound edges */
} CONN;


/** Key-paths data */
typedef struct keypaths_data_structures
{
   int* const            kpnodes;            /**< key path nodes */
   int* const            kpedges;            /**< key path edges */
   SCIP_Real             kpcost;             /**< cost of key paths */
   int                   nkpnodes;           /**< number of key path nodes */
   int                   nkpedges;           /**< number of key path edges */
   int                   rootpathstart;      /**< start of key path towards root component */
   int                   kptailnode;         /**< needed for single path */
} KPATHS;


/** Solution tree data */
typedef struct solution_tree_data
{
   STP_Bool* const       solNodes;           /**< Steiner tree nodes */
   NODE* const           linkcutNodes;       /**< Steiner tree nodes */
   int* const            solEdges;           /**< array indicating whether an arc is part of the solution (CONNECTED/UNKNOWN) */
   STP_Bool* const       nodeIsPinned;       /**< of size nodes */
   STP_Bool* const       nodeIsScanned;      /**< of size nodes */
   int* const            newedges;           /**< marks new edges of the tree */
} SOLTREE;


/** Super graph data */
typedef struct supergraph_data
{
   int* const            supernodes;         /**< super nodes */
   STP_Bool* const       nodeIsSupernode;    /**< marks the current super-vertices
                                              * (except for the one representing the root-component) */
   PATH*                 mst;                /**< MST */
   SCIP_Real             mstcost;            /**< cost of MST */
   int                   nsupernodes;        /**< number of super nodes */
} SGRAPH;


/** Prize-collecting/maximum-weight connected subgraph data */
typedef struct pcmw_data
{
   SCIP_Real* const      prize_biased;       /**< prize */
   SCIP_Real* const      edgecost_biased;    /**< cost */
   STP_Bool* const       prizemark;          /**< marked? */
   int* const            prizemarklist;      /**< list of all marked */
} PCMW;

/*
 * Local methods
 */

/** recursive methode for a DFS ordering of graph 'g' */
static
void dfsorder(
   const GRAPH*          graph,
   const int*            edges,
   int                   node,
   int*                  counter,
   int*                  dfst
   )
{
   int oedge = graph->outbeg[node];

   while( oedge >= 0 )
   {
      if( edges[oedge] >= 0 )
      {
         dfsorder(graph, edges, graph->head[oedge], counter, dfst);
      }
      oedge = graph->oeat[oedge];
   }

   dfst[*counter] = node;
   (*counter)++;
}


static inline
SCIP_Real getNewPrizeNode(
   const GRAPH*          graph,
   const STP_Bool*       steinertree,
   const int*            graphmark,
   int                   node,
   STP_Bool*             prizemark,
   int*                  prizemarklist,
   int*                  prizemarkcount
   )
{
   SCIP_Real prizesum = 0.0;
   assert(graph_pc_isPcMw(graph));

   if( graphmark[node] && !steinertree[node] && Is_pterm(graph->term[node]) && !prizemark[node] )
   {
      prizesum += graph->prize[node];
      prizemark[node] = TRUE;
      prizemarklist[(*prizemarkcount)++] = node;
   }

   return prizesum;
}

static
SCIP_Real getNewPrize(
   const GRAPH*          graph,
   const STP_Bool*       steinertree,
   const int*            graphmark,
   int                   edge,
   STP_Bool*             prizemark,
   int*                  prizemarklist,
   int*                  prizemarkcount
   )
{
   SCIP_Real prizesum = 0.0;

   if( graph_pc_isPcMw(graph) )
   {
      const int mhead = graph->head[edge];
      const int mtail = graph->tail[edge];

      prizesum += getNewPrizeNode(graph, steinertree, graphmark, mhead, prizemark, prizemarklist, prizemarkcount);
      prizesum += getNewPrizeNode(graph, steinertree, graphmark, mtail, prizemark, prizemarklist, prizemarkcount);
   }

   return prizesum;
}


/** computes lowest common ancestors for all pairs {vbase(v), vbase(u)} such that {u,w} is a boundary edge,
 * first call should be with u := root */
static
SCIP_RETCODE lca(
   SCIP*                 scip,
   const GRAPH*          graph,
   int                   u,
   UF*                   uf,
   STP_Bool*             nodesmark,
   const int*            steineredges,
   IDX**                 lcalists,
   PHNODE**              boundpaths,
   const int*            heapsize,
   const int*            vbase
   )
{
   int* uboundpaths; /* boundary-paths (each one represented by its boundary edge) having node 'u' as an endpoint */
   uf->parent[u] = u;

   for( int oedge = graph->outbeg[u]; oedge != EAT_LAST; oedge = graph->oeat[oedge] )
   {
      const int v = graph->head[oedge];
      if( steineredges[oedge] == CONNECT )
      {
         SCIP_CALL( lca(scip, graph, v, uf, nodesmark, steineredges, lcalists, boundpaths, heapsize, vbase) );
         SCIPStpunionfindUnion(uf, u, v, FALSE);
         uf->parent[SCIPStpunionfindFind(uf, u)] = u;
      }
   }

   nodesmark[u] = TRUE;

   /* iterate through all boundary-paths having one endpoint in the voronoi region of node 'u' */
   SCIP_CALL( SCIPpairheapBuffarr(scip, boundpaths[u], heapsize[u], &uboundpaths) );

   for( int i = 0; i < heapsize[u]; i++ )
   {
      const int oedge = uboundpaths[i];
      const int v = vbase[graph->head[oedge]];
      if( nodesmark[v] )
      {
         const int ancestor = uf->parent[SCIPStpunionfindFind(uf, v)];

         /* if the ancestor of 'u' and 'v' is one of the two, the boundary-edge is already in boundpaths[u] */
         if( ancestor != u && ancestor != v)
         {
            IDX* curr;

            SCIP_CALL( SCIPallocBlockMemory(scip, &curr) );
            curr->index = oedge;
            curr->parent = lcalists[ancestor];
            lcalists[ancestor] = curr;
         }
      }
   }

   /* free the boundary-paths array */
   SCIPfreeBufferArray(scip, &uboundpaths);

   return SCIP_OKAY;
}


/** computes lowest common ancestors for all pairs {vbase(v), vbase(u)} such that {u,w} is a boundary edge */
static
SCIP_RETCODE getLowestCommonAncestors(
   SCIP*                 scip,
   const GRAPH*          graph,
   const VNOI*           vnoiData,           /**< Voronoi data */
   const SOLTREE*        soltreeData,        /**< solution tree data */
   CONN*                 connectData         /**< data */
)
{
   PHNODE** const boundpaths = connectData->pheap_boundpaths;
   IDX** const lvledges_start = connectData->lvledges_start;
   const int* const solEdges = soltreeData->solEdges;
   int* const pheapsize = connectData->pheap_sizes;
   const int* const vnoibase = vnoiData->vnoi_base;
   STP_Bool* nodesmark;
   const int nnodes = graph->knots;

   assert(SCIPStpunionfindIsClear(scip, connectData->uf));

   SCIP_CALL( SCIPallocBufferArray(scip, &nodesmark, nnodes) );

   for( int i = 0; i < nnodes; ++i )
      nodesmark[i] = FALSE;

   SCIP_CALL( lca(scip, graph, graph->source, connectData->uf, nodesmark, solEdges, lvledges_start, boundpaths, pheapsize, vnoibase) );

   SCIPfreeBufferArray(scip, &nodesmark);

   return SCIP_OKAY;
}

/** submethod for local extend */
static
SCIP_RETCODE addToCandidates(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const PATH*           path,               /**< shortest data structure array */
   int                   i,                  /**< node */
   int                   greedyextensions,   /**< greedyextensions */
   int*                  nextensions,        /**< nextensions */
   GNODE*                candidates,         /**< candidates */
   SCIP_PQUEUE*          pqueue              /**< pqueue */
   )
{

   assert(!graph_pc_knotIsFixedTerm(graph, i));

   if( *nextensions < greedyextensions )
   {
      candidates[*nextensions].dist = graph->prize[i] - path[i].dist;
      candidates[*nextensions].number = i;

      SCIP_CALL( SCIPpqueueInsert(pqueue, &(candidates[(*nextensions)++])) );
   }
   else
   {
      /* get candidate vertex of minimum value */
      GNODE* min = (GNODE*) SCIPpqueueFirst(pqueue);
      if( SCIPisLT(scip, min->dist, graph->prize[i] - path[i].dist) )
      {
         min = (GNODE*) SCIPpqueueRemove(pqueue);
         min->dist = graph->prize[i] - path[i].dist;
         min->number = i;
         SCIP_CALL( SCIPpqueueInsert(pqueue, min) );
      }
   }

   return SCIP_OKAY;
}


/** checks whether node is crucial, i.e. a terminal or a vertex with degree at least 3 (w.r.t. the steinertree) */
static
STP_Bool nodeIsCrucial(
   const GRAPH*          graph,              /**< graph data structure */
   const int*            steineredges,
   int                   node
)
{
   int todo; // adapt for small prizes

   assert(graph != NULL);
   assert(steineredges != NULL);
   if( graph->term[node] == -1 )
   {
      int counter = 0;
      int e = graph->outbeg[node];
      while( e >= 0 )
      {
         /* check if the adjacent node is in the ST */
         if( steineredges[e] > -1 || steineredges[flipedge(e)] > -1 )
         {
            counter++;
         }
         e = graph->oeat[e];
      }

      if( counter < 3 )
      {
         return FALSE;
      }
   }

   return TRUE;
}


/** is given Steiner tree a trivial solution (i.e. contains only one vertex?) */
static
SCIP_Bool solIsTrivialPcMw(
   const GRAPH*          graph,              /**< graph data structure */
   const int*            solEdges            /**< Steiner tree edges */
)
{
   const int root = graph->source;
   SCIP_Bool isTrivial = TRUE;

   assert(graph_pc_isPcMw(graph));
   assert(graph->extended);

   if( graph_pc_isRootedPcMw(graph) )
   {
      for( int e = graph->outbeg[root]; e != EAT_LAST; e = graph->oeat[e] )
      {
         if( solEdges[e] )
         {
            const int head = graph->head[e];
            if( graph_pc_knotIsFixedTerm(graph, head) || !Is_term(graph->term[head]) )
            {
               isTrivial = FALSE;
               break;
            }
         }
      }
   }
   else
   {
      isTrivial = FALSE;
   }


   if( isTrivial )
   {
      SCIPdebugMessage("trivial solution given \n");
   }

   return isTrivial;
}


/** perform local vertex insertion heuristic on given Steiner tree */
static
void markSolTreeNodes(
   const GRAPH*          graph,              /**< graph data structure */
   const int*            solEdges,           /**< Steiner tree edges */
   NODE*                 linkcutNodes,       /**< Steiner tree nodes */
   STP_Bool*             solNodes            /**< Steiner tree nodes */
   )
{
   const int nnodes = graph->knots;
   const int nedges = graph->edges;

   for( int i = 0; i < nnodes; i++ )
   {
      solNodes[i] = FALSE;
      SCIPlinkcuttreeInit(&linkcutNodes[i]);
   }

   /* create a link-cut tree representing the current Steiner tree */
   for( int e = 0; e < nedges; e++ )
      if( solEdges[e] == CONNECT )
         SCIPlinkcuttreeLink(&linkcutNodes[graph->head[e]], &linkcutNodes[graph->tail[e]], flipedge(e));

   /* mark current Steiner tree nodes */
   for( int e = 0; e < nedges; e++ )
   {
      if( solEdges[e] == CONNECT )
      {
         solNodes[graph->tail[e]] = TRUE;
         solNodes[graph->head[e]] = TRUE;
      }
   }
}



/** get key paths starting from given key vertex */
/** gets cost of shortest path along boundary edge*/
static
SCIP_Real vnoiGetBoundaryPathCost(
   const GRAPH*          graph,              /**< graph data structure */
   const VNOI*           vnoiData,           /**< data */
   const PCMW*           pcmwData,           /**< data */
   int                   boundaryedge        /**< boundary edge*/
   )
{
   const PATH* const vnoipath = vnoiData->vnoi_path;
   SCIP_Real pathcost;
   const int node = graph->tail[boundaryedge];
   const int adjnode = graph->head[boundaryedge];

   assert(boundaryedge >= 0);
   assert(vnoiData->vnoi_base[node] != vnoiData->vnoi_base[adjnode]);

   pathcost = vnoipath[node].dist + graph->cost[boundaryedge] + vnoipath[adjnode].dist;
   assert(pathcost >= 0.0);

   return pathcost;
}


/** initialize for PC/MW */
static
void pcmwInit(
   SCIP*                 scip,               /**< SCIP data structure */
   GRAPH*                graph,              /**< graph data structure */
   SOLTREE*              soltreeData,        /**< solution tree data */
   PCMW*                 pcmwData            /**< data */
   )
{
   STP_Bool *const pinned = soltreeData->nodeIsPinned;
   const int root = graph->source;
   int* const graphmark = graph->mark;

   assert(graph->extended);
   assert(graph_pc_isPcMw(graph));

   graph_pc_getBiased(scip, graph, TRUE, pcmwData->edgecost_biased, pcmwData->prize_biased);

   for( int e = graph->outbeg[root]; e != EAT_LAST; e = graph->oeat[e] )
   {
      const int k = graph->head[e];
      if( Is_term(graph->term[k]) )
      {
         if( !graph_pc_knotIsFixedTerm(graph, k) )
         {
            const int pterm = graph->head[graph->term2edge[k]];

            assert(Is_pterm(graph->term[pterm]));

            graphmark[k] = FALSE;
            pinned[pterm] = TRUE;
         }
      }
   }

   if( !graph_pc_isRootedPcMw(graph) )
      graphmark[root] = FALSE;
}


/** update for key-vertex elimination */
static
SCIP_RETCODE connectivityDataKeyElimUpdate(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const VNOI*           vnoiData,           /**< Voronoi data */
   const SGRAPH*         supergraphData,     /**< super-graph*/
   int                   crucnode,           /**< node to eliminate */
   CONN*                 connectData         /**< data */
)
{
   PHNODE** const boundpaths = connectData->pheap_boundpaths;
   IDX** const lvledges_start = connectData->lvledges_start;
   UF* const uf = connectData->uf;
   int* const pheapsize = connectData->pheap_sizes;
   const int* const supernodes = supergraphData->supernodes;
   const int* const vnoibase = vnoiData->vnoi_base;
   const STP_Bool* const isSupernode = supergraphData->nodeIsSupernode;
   int* const boundedges = connectData->boundedges;
   const int* const graphmark = graph->mark;
   int nboundedges = 0;

   connectData->nboundedges = -1;

   /* add vertical boundary-paths between the child components and the root-component (w.r.t. node 'crucnode') */
   for( int k = 0; k < supergraphData->nsupernodes - 1; k++ )
   {
      const int supernode = supernodes[k];
      int edge = UNKNOWN;

      while( boundpaths[supernode] != NULL )
      {
         int node;
         SCIP_Real edgecost;

         SCIP_CALL( SCIPpairheapDeletemin(scip, &edge, &edgecost, &boundpaths[supernode], &pheapsize[supernode]) );

         node = (vnoibase[graph->head[edge]] == UNKNOWN)? UNKNOWN : SCIPStpunionfindFind(uf, vnoibase[graph->head[edge]]);

         /* check whether edge 'edge' represents a boundary-path having an endpoint in the kth-component and in the root-component respectively */
         if( node != UNKNOWN && !isSupernode[node] && graphmark[node] )
         {
            boundedges[nboundedges++] = edge;
            SCIP_CALL( SCIPpairheapInsert(scip, &boundpaths[supernode], edge, edgecost, &pheapsize[supernode]) );
            break;
         }
      }
   }

   /* add horizontal boundary-paths (between the  child-components) */
   for( IDX* lvledges_curr = lvledges_start[crucnode]; lvledges_curr != NULL; lvledges_curr = lvledges_curr->parent )
   {
      const int edge = lvledges_curr->index;
      const int basetail = vnoibase[graph->tail[edge]];
      const int basehead = vnoibase[graph->head[edge]];
      const int node = (basehead == UNKNOWN)? UNKNOWN : SCIPStpunionfindFind(uf, basehead);
      const int adjnode = (basetail == UNKNOWN)? UNKNOWN : SCIPStpunionfindFind(uf, basetail);

      /* check whether the current boundary-path connects two child components */
      if( node != UNKNOWN && isSupernode[node] && adjnode != UNKNOWN && isSupernode[adjnode] )
      {
         assert(graphmark[node] && graphmark[adjnode]);
         boundedges[nboundedges++] = edge;
      }
   }

   connectData->nboundedges = nboundedges;

   return SCIP_OKAY;
}


/** initialize */
static
SCIP_RETCODE connectivityDataInit(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const VNOI*           vnoiData,           /**< Voronoi data */
   const SOLTREE*        soltreeData,        /**< solution tree data */
   const PCMW*           pcmwData,           /**< data */
   CONN*                 connectData         /**< data */
)
{
   PHNODE** const boundpaths = connectData->pheap_boundpaths;
   IDX** const blists_start = connectData->blists_start;
   IDX** const lvledges_start = connectData->lvledges_start;
   int* const pheapsize = connectData->pheap_sizes;
   const int* const vnoibase = vnoiData->vnoi_base;
   const int* const graphmark = graph->mark;
   const int nnodes = graph->knots;
   const int nedges = graph->edges;

   assert(connectData->nboundedges == 0);
   assert(connectData->boundedges);

   BMSclearMemoryArray(blists_start, nnodes);

   for( int k = 0; k < nnodes; ++k )
   {
      IDX* blists_curr;

      /* initialize pairing heaps */
      pheapsize[k] = 0;
      boundpaths[k] = NULL;

      lvledges_start[k] = NULL;

      if( !graphmark[k] )
         continue;

      /* link all nodes to their (respective) Voronoi base */
      SCIP_CALL( SCIPallocBlockMemory(scip, &blists_curr) );
      blists_curr->index = k;
      blists_curr->parent = blists_start[vnoibase[k]];
      blists_start[vnoibase[k]] = blists_curr;
   }


   /* for each node, store all of its outgoing boundary-edges in a (respective) heap*/
   for( int e = 0; e < nedges; e += 2 )
   {
      if( graph->oeat[e] != EAT_FREE )
      {
         const int node = graph->tail[e];
         const int adjnode = graph->head[e];

         /* is edge 'e' a boundary-edge? */
         if( vnoibase[node] != vnoibase[adjnode] && graphmark[node] && graphmark[adjnode] )
         {
            const SCIP_Real edgecost = vnoiGetBoundaryPathCost(graph, vnoiData, pcmwData, e);

            assert(SCIPisGE(scip, edgecost, 0.0));

            /* add the boundary-edge 'e' and its reversed to the corresponding heaps */
            SCIP_CALL( SCIPpairheapInsert(scip, &boundpaths[vnoibase[node]], e, edgecost, &(pheapsize[vnoibase[node]])) );
            SCIP_CALL( SCIPpairheapInsert(scip, &boundpaths[vnoibase[adjnode]], flipedge(e), edgecost, &(pheapsize[vnoibase[adjnode]])) );
         }
      }
   }

   SCIP_CALL( getLowestCommonAncestors(scip, graph, vnoiData, soltreeData, connectData) );

   return SCIP_OKAY;
}


/** get key path above given crucial node */
static
void getKeyPathUpper(
   SCIP*                 scip,               /**< SCIP data structure */
   int                   crucnode,           /**< crucial node to start from */
   const GRAPH*          graph,              /**< graph data structure */
   const SOLTREE*        soltreeData,        /**< solution tree data */
   CONN*                 connectData,        /**< data */
   KPATHS*               keypathsData        /**< key paths */
)
{
   int* const kpnodes = keypathsData->kpnodes;
   const int* const solEdges = soltreeData->solEdges;
   const NODE* const linkcutNodes = soltreeData->linkcutNodes;
   const STP_Bool* const solNodes = soltreeData->solNodes;
   const STP_Bool* const pinned = soltreeData->nodeIsPinned;
   PHNODE** const boundpaths = connectData->pheap_boundpaths;
   int* const pheapsize = connectData->pheap_sizes;
   const int* const graphmark = graph->mark;
   int nkpnodes = 0;
   int kptailnode = -1;
   SCIP_Real kpcost = -FARAWAY;

   if( Is_term(graph->term[crucnode]) || pinned[crucnode] )
   {
      for( int edge = graph->outbeg[crucnode]; edge != EAT_LAST; edge = graph->oeat[edge] )
      {
         int adjnode = graph->head[edge];

         /* check whether edge 'edge' leads to an ancestor of terminal 'crucnode' */
         if( solEdges[edge] == CONNECT && solNodes[adjnode] && graphmark[adjnode] )
         {
            assert( SCIPStpunionfindFind(connectData->uf, adjnode) != crucnode);
            assert(soltreeData->nodeIsScanned[adjnode]);

            SCIPpairheapMeldheaps(scip, &boundpaths[crucnode], &boundpaths[adjnode], &pheapsize[crucnode], &pheapsize[adjnode]);

            /* update the union-find data structure */
            SCIPStpunionfindUnion(connectData->uf, crucnode, adjnode, FALSE);

            /* move along the key-path until its end (i.e. until a crucial node is reached) */
            while( !nodeIsCrucial(graph, solEdges, adjnode) && !pinned[adjnode] )
            {
               int e;
               for( e = graph->outbeg[adjnode]; e != EAT_LAST; e = graph->oeat[e] )
                  if( solEdges[e] != -1 )
                     break;

               /* assert that each leaf of the ST is a terminal */
               assert( e != EAT_LAST );
               adjnode = graph->head[e];

               if( !solNodes[adjnode] || !graphmark[adjnode] )
                  break;

               assert(soltreeData->nodeIsScanned[adjnode]);
               assert(SCIPStpunionfindFind(connectData->uf, adjnode) != crucnode);

               /* update the union-find data structure */
               SCIPStpunionfindUnion(connectData->uf, crucnode, adjnode, FALSE);

               /* meld the heaps */
               SCIPpairheapMeldheaps(scip, &boundpaths[crucnode], &boundpaths[adjnode], &pheapsize[crucnode], &pheapsize[adjnode]);
            }
         }
      }
   }

#ifndef NDEBUG
   if( SCIPisGE(scip, graph->cost[linkcutNodes[crucnode].edge], FARAWAY)
      || SCIPisGE(scip, graph->cost[flipedge(linkcutNodes[crucnode].edge)], FARAWAY) )
   {
      assert(graph_pc_isPcMw(graph));
      assert(graph->head[linkcutNodes[crucnode].edge] == graph->source);
   }
#endif

   /* find the (unique) key-path containing the parent of the current crucial node 'crucnode' */
   kptailnode = graph->head[linkcutNodes[crucnode].edge];
   kpcost = graph->cost[linkcutNodes[crucnode].edge];

#ifdef SCIP_DEBUG
   printf("key path edge ");
   graph_edge_printInfo(graph, linkcutNodes[crucnode].edge);
#endif

   while( !nodeIsCrucial(graph, solEdges, kptailnode) && !pinned[kptailnode] )
   {
      const int kpedge = linkcutNodes[kptailnode].edge;
      kpcost += graph->cost[kpedge];

#ifdef SCIP_DEBUG
      printf("key path edge ");
      graph_edge_printInfo(graph, kpedge);
#endif

      kpnodes[nkpnodes++] = kptailnode;
      kptailnode = graph->head[kpedge];
   }

   keypathsData->kpcost = kpcost;
   keypathsData->kptailnode = kptailnode;
   keypathsData->nkpnodes = nkpnodes;
}


/** exchanges key path */
static
SCIP_RETCODE soltreeExchangeKeyPath(
   SCIP*                 scip,               /**< SCIP data structure */
   GRAPH*                graph,              /**< graph data structure */
   const CONN*           connectData,        /**< data */
   const VNOI*           vnoiData,           /**< data */
   const KPATHS*         keypathsData,       /**< key paths */
   const int*            dfstree,            /**< DFS tree */
   const STP_Bool*       scanned,            /**< array to mark which nodes have been scanned */
   int                   dfstree_pos,        /**< current position in dfs tree */
   int                   boundedge_new,      /**< new Voronoi boundary edge */
   SOLTREE*              soltreeData         /**< solution tree data */
)
{
   UF* const uf = connectData->uf;
   PHNODE** const boundpaths = connectData->pheap_boundpaths;
   int* const pheapsize = connectData->pheap_sizes;
   const PATH* const vnoipath = vnoiData->vnoi_path;
   const int* const vnoibase = vnoiData->vnoi_base;
   const int* const kpnodes = keypathsData->kpnodes;
   STP_Bool* const pinned = soltreeData->nodeIsPinned;
   const NODE* const linkcutNodes = soltreeData->linkcutNodes;
   int* const solEdges = soltreeData->solEdges;
   STP_Bool* const solNodes = soltreeData->solNodes;
   const int nkpnodes = keypathsData->nkpnodes;
   const int crucnode = dfstree[dfstree_pos];
   int* const graphmark = graph->mark;
   int newpathend = -1;
   int newedge = boundedge_new;
   int node = SCIPStpunionfindFind(uf, vnoibase[graph->head[newedge]]);

   /* remove old keypath */
   assert(solEdges[flipedge(linkcutNodes[crucnode].edge)] != UNKNOWN);

   solEdges[flipedge(linkcutNodes[crucnode].edge)] = UNKNOWN;
   solNodes[crucnode] = FALSE;
   graphmark[crucnode] = FALSE;

   for( int k = 0; k < nkpnodes; k++ )
   {
      const int keypathnode = kpnodes[k];
      assert(solEdges[flipedge(linkcutNodes[keypathnode].edge)] != UNKNOWN);

      solEdges[flipedge(linkcutNodes[keypathnode].edge)] = UNKNOWN;
      solNodes[keypathnode] = FALSE;
      graphmark[keypathnode] = FALSE;
   }

   assert(graphmark[keypathsData->kptailnode]);

   if( node == crucnode )
      newedge = flipedge(newedge);

   for( node = graph->tail[newedge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
   {
      graphmark[node] = FALSE;

      solEdges[flipedge(vnoipath[node].edge)] = CONNECT;
      solEdges[vnoipath[node].edge] = UNKNOWN;
   }

   for( node = graph->head[newedge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
   {
      graphmark[node] = FALSE;

      solEdges[vnoipath[node].edge] = CONNECT;
   }

   solEdges[flipedge(newedge)] = CONNECT;

   newpathend = vnoibase[graph->tail[newedge]];
   assert(node == vnoibase[graph->head[newedge]] );

   /* flip all edges on the ST path between the endnode of the new key-path and the current crucial node */
   assert(SCIPStpunionfindFind(uf, newpathend) == crucnode);

   for( int k = newpathend; k != crucnode; k = graph->head[linkcutNodes[k].edge] )
   {
      assert(graphmark[k]);
      assert( solEdges[flipedge(linkcutNodes[k].edge)] != -1);

      solEdges[flipedge(linkcutNodes[k].edge)] = UNKNOWN;
      solEdges[linkcutNodes[k].edge] = CONNECT;
   }

   for( int k = 0; k < dfstree_pos; k++ )
   {
      if( crucnode == SCIPStpunionfindFind(uf, dfstree[k]) )
      {
         graphmark[dfstree[k]] = FALSE;
         solNodes[dfstree[k]] = FALSE;
      }
   }

   /* update union find */
   if( !Is_term(graph->term[node]) && scanned[node] && !pinned[node] && SCIPStpunionfindFind(uf, node) == node )
   {
      for( int edge = graph->outbeg[node]; edge != EAT_LAST; edge = graph->oeat[edge] )
      {
         int adjnode = graph->head[edge];

         /* check whether edge 'edge' leads to an ancestor of terminal 'node' */
         if( solEdges[edge] == CONNECT && solNodes[adjnode] && graphmark[adjnode] && SCIPStpunionfindFind(uf, adjnode) != node )
         {
            assert(scanned[adjnode]);
            /* meld the heaps */
            SCIPpairheapMeldheaps(scip, &boundpaths[node], &boundpaths[adjnode], &pheapsize[node], &pheapsize[adjnode]);

            /* update the union-find data structure */
            SCIPStpunionfindUnion(uf, node, adjnode, FALSE);

            /* move along the key-path until its end (i.e. until a crucial node is reached) */
            while( !nodeIsCrucial(graph, solEdges, adjnode) && !pinned[adjnode] )
            {
               int e;
               for( e = graph->outbeg[adjnode]; e != EAT_LAST; e = graph->oeat[e] )
               {
                  if( solEdges[e] != -1 )
                     break;
               }

               /* assert that each leaf of the ST is a terminal */
               assert( e != EAT_LAST );
               adjnode = graph->head[e];

               if( !solNodes[adjnode]  )
                  break;

               assert(scanned[adjnode]);
               assert(SCIPStpunionfindFind(uf, adjnode) != node);

               /* update the union-find data structure */
               SCIPStpunionfindUnion(uf, node, adjnode, FALSE);

               /* meld the heaps */
               SCIPpairheapMeldheaps(scip, &boundpaths[node], &boundpaths[adjnode], &pheapsize[node], &pheapsize[adjnode]);
            }
         }
      }

   }
   pinned[node] = TRUE;

   return SCIP_OKAY;
}



/** exchanges key-paths star */
static
SCIP_RETCODE soltreeElimKeyPathsStar(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const CONN*           connectData,        /**< data */
   const VNOI*           vnoiData,           /**< data */
   const KPATHS*         keypathsData,       /**< key paths */
   const SGRAPH*         supergraphData,     /**< super-graph */
   const int*            dfstree,            /**< DFS tree */
   const STP_Bool*       scanned,            /**< array to mark which nodes have been scanned */
   int                   dfstree_pos,        /**< current position in dfs tree */
   SOLTREE*              soltreeData         /**< solution tree data */
)
{
   const PATH* mst = supergraphData->mst;
   UF* const uf = connectData->uf;
   const STP_Bool* const isSupernode = supergraphData->nodeIsSupernode;
   PHNODE** const boundpaths = connectData->pheap_boundpaths;
   int* const pheapsize = connectData->pheap_sizes;
   const PATH* const vnoipath = vnoiData->vnoi_path;
   const int* const vnoibase = vnoiData->vnoi_base;
   const int* const boundedges = connectData->boundedges;
   const int* const kpnodes = keypathsData->kpnodes;
   const int* const kpedges = keypathsData->kpedges;
   STP_Bool* const pinned = soltreeData->nodeIsPinned;
   const NODE* const linkcutNodes = soltreeData->linkcutNodes;
   int* const solEdges = soltreeData->solEdges;
   int* const graphmark = graph->mark;
   STP_Bool* const solNodes = soltreeData->solNodes;
   const int nkpnodes = keypathsData->nkpnodes;
   const int nkpedges = keypathsData->nkpedges;
   const int nsupernodes = supergraphData->nsupernodes;

   /* unmark the original edges spanning the supergraph */
   for( int e = 0; e < nkpedges; e++ )
   {
      assert(solEdges[kpedges[e]] != -1);
      solEdges[kpedges[e]] = -1;
   }

   /* mark all ST nodes except for those belonging to the root-component as forbidden */
   for( int k = keypathsData->rootpathstart; k < nkpnodes; k++ )
   {
      graphmark[kpnodes[k]] = FALSE;
      solNodes[kpnodes[k]] = FALSE;
   }

   for( int k = 0; k < dfstree_pos; k++ )
   {
      const int node = SCIPStpunionfindFind(uf, dfstree[k]);
      if( isSupernode[node] || node == dfstree[dfstree_pos] )
      {
         graphmark[dfstree[k]] = FALSE;
         solNodes[dfstree[k]] = FALSE;
      }
   }

   /* add the new edges reconnecting the (super-) components */
   for( int l = 0; l < nsupernodes - 1; l++ )
   {
      int edge;

      if( mst[l].edge % 2  == 0 )
         edge = boundedges[mst[l].edge / 2 ];
      else
         edge = flipedge(boundedges[mst[l].edge / 2 ]);

      /* change the orientation within the target-component if necessary */
      if( !isSupernode[vnoibase[graph->head[edge]]] )
      {
         int node = vnoibase[graph->head[edge]];
         const int nodebase = SCIPStpunionfindFind(uf, node);
         assert(isSupernode[nodebase]);

         while( node != nodebase )
         {
            /* the ST edge pointing towards the root */
            const int e = linkcutNodes[node].edge;

            assert(solEdges[e] == -1 && solEdges[flipedge(e)] != -1 );
            solEdges[e] = CONNECT;
            solEdges[flipedge(e)] = UNKNOWN;
            node = graph->head[e];
         }
      }

      /* is the vbase of the current boundary-edge tail in the root-component? */
      if( !isSupernode[SCIPStpunionfindFind(uf, vnoibase[graph->tail[edge]])] )
      {
         int node;
         solEdges[edge] = CONNECT;

         for( node = graph->tail[edge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
         {
            graphmark[node] = FALSE;

            if( solEdges[flipedge(vnoipath[node].edge)] == CONNECT )
               solEdges[flipedge(vnoipath[node].edge)] = UNKNOWN;

            solEdges[vnoipath[node].edge] = CONNECT;
         }

         assert(!isSupernode[node] && vnoibase[node] == node);
         assert( graphmark[node] == TRUE );

         /* is the pinned node its own component identifier? */
         if( !Is_term(graph->term[node]) && scanned[node] && !pinned[node] && SCIPStpunionfindFind(uf, node) == node )
         {
            const int oldedge = edge;

            graphmark[graph->head[edge]] = FALSE;

            for( edge = graph->outbeg[node]; edge != EAT_LAST; edge = graph->oeat[edge] )
            {
               int head = graph->head[edge];

               /* check whether edge 'edge' leads to an ancestor of terminal 'node' */
               if( solEdges[edge] == CONNECT && graphmark[head] && solNodes[head]  && SCIPStpunionfindFind(uf, head) != node )
               {

                  assert(scanned[head]);
                  /* meld the heaps */
                  SCIPpairheapMeldheaps(scip, &boundpaths[node], &boundpaths[head], &pheapsize[node], &pheapsize[head]);

                  /* update the union-find data structure */
                  SCIPStpunionfindUnion(uf, node, head, FALSE);

                  /* move along the key-path until its end (i.e. until a crucial node is reached) */
                  while( !nodeIsCrucial(graph, solEdges, head) && !pinned[head] )
                  {
                     int e;
                     for( e = graph->outbeg[head]; e != EAT_LAST; e = graph->oeat[e] )
                     {
                        if( solEdges[e] != -1 )
                           break;
                     }

                     /* assert that each leaf of the ST is a terminal */
                     assert( e != EAT_LAST );
                     head = graph->head[e];

                     if( !solNodes[head]  )
                        break;

                     assert(scanned[head]);
                     assert(SCIPStpunionfindFind(uf, head) != node);

                     /* update the union-find data structure */
                     SCIPStpunionfindUnion(uf, node, head, FALSE);

                     /* meld the heaps */
                     SCIPpairheapMeldheaps(scip, &boundpaths[node], &boundpaths[head], &pheapsize[node], &pheapsize[head]);
                  }
               }
            }
            edge = oldedge;
         }

         /* mark the start node (lying in the root-component of the ST) of the current boundary-path as pinned,
          * so that it may not be removed later on */
         pinned[node] = TRUE;

         for( node = graph->head[edge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
         {
            graphmark[node] = FALSE;
            if( solEdges[vnoipath[node].edge] == CONNECT )
               solEdges[vnoipath[node].edge] = -1;

            solEdges[flipedge(vnoipath[node].edge)] = CONNECT;
         }
      }
      else
      {

         solEdges[edge] = CONNECT;

         for( int node = graph->tail[edge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
         {
            graphmark[node] = FALSE;
            if( solEdges[vnoipath[node].edge] != CONNECT && solEdges[flipedge(vnoipath[node].edge)] != CONNECT )
            {
               solEdges[vnoipath[node].edge] = CONNECT;
            }
         }

         for( int node = graph->head[edge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
         {
            graphmark[node] = FALSE;

            solEdges[flipedge(vnoipath[node].edge)] = CONNECT;
            solEdges[vnoipath[node].edge] = UNKNOWN;
         }
      }
   }

   for( int k = 0; k < nkpnodes; k++ )
   {
      assert(graphmark[kpnodes[k]] == FALSE);
      assert(solNodes[kpnodes[k]] == FALSE);
   }

   assert(!graphmark[dfstree[dfstree_pos]]);

   return SCIP_OKAY;
}


/** compute cost of alternative key path */
static
SCIP_Real getKeyPathReplaceCost(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const VNOI*           vnoiData,           /**< data */
   const PCMW*           pcmwData,           /**< data */
   const SOLTREE*        soltreeData,        /**< solution tree data */
   SCIP_Real             edgecost_initial,   /**< initial edge cost */
   int                   boundedge_old,      /**< Voronoi boundary edge */
   int*                  boundedge_new       /**< new Voronoi boundary edge */
)
{
   SCIP_Real edgecost = edgecost_initial;
   int newedge = *boundedge_new;

   if( boundedge_old != UNKNOWN && newedge != UNKNOWN
      && SCIPisLT(scip, edgecost, vnoiGetBoundaryPathCost(graph, vnoiData, pcmwData, newedge)) )
   {
      assert(SCIPisGE(scip, edgecost, 0.0));
      newedge = boundedge_old;
   }

   if( boundedge_old != UNKNOWN && newedge == UNKNOWN )
      newedge = boundedge_old;

   assert( newedge != UNKNOWN );

   edgecost = vnoiGetBoundaryPathCost(graph, vnoiData, pcmwData, newedge);;

   if( graph_pc_isPcMw(graph) )
   {
      const STP_Bool* const solNodes = soltreeData->solNodes;
      const PATH* const vnoipath = vnoiData->vnoi_path;
      const int* const vnoibase = vnoiData->vnoi_base;
      const int* const graphmark = graph->mark;
      STP_Bool* const prizemark = pcmwData->prizemark;
      int* const prizemarklist = pcmwData->prizemarklist;
      int prizemarkcount = 0;

#ifndef NDEBUG
      for( int k = 0; k < graph->knots; k++ )
         assert(!prizemark[k]);
#endif
#ifdef SCIP_DEBUG
      printf("key path alternative edge ");
      graph_edge_printInfo(graph, newedge);
#endif
      edgecost -= getNewPrize(graph, solNodes, graphmark, newedge, prizemark, prizemarklist, &prizemarkcount);

      for( int node = graph->tail[newedge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
      {
#ifdef SCIP_DEBUG
         printf("key path alternative edge ");
         graph_edge_printInfo(graph, vnoipath[node].edge);
#endif
         edgecost -= getNewPrize(graph, solNodes, graphmark, vnoipath[node].edge, prizemark, prizemarklist, &prizemarkcount);
      }

      for( int node = graph->head[newedge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
      {
#ifdef SCIP_DEBUG
         printf("key path alternative edge ");
         graph_edge_printInfo(graph, vnoipath[node].edge);
#endif

         edgecost -= getNewPrize(graph, solNodes, graphmark, vnoipath[node].edge, prizemark, prizemarklist, &prizemarkcount);
      }

      for( int pi = 0; pi < prizemarkcount; pi++ )
         prizemark[prizemarklist[pi]] = FALSE;
   }

   *boundedge_new = newedge;

   return edgecost;
}


/** compute minimum-spanning tree  */
static
SCIP_RETCODE supergraphComputeMst(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const CONN*           connectData,        /**< data */
   const SOLTREE*        soltreeData,        /**< solution tree data */
   const VNOI*           vnoiData,           /**< data */
   const PCMW*           pcmwData,           /**< data */
   int                   crucnode,           /**< node to eliminate */
   KPATHS*               keypathsData,       /**< key paths */
   SGRAPH*               supergraphData      /**< super-graph*/
)
{
   PATH* mst = NULL;
   GRAPH* supergraph = NULL;
   UF* const uf = connectData->uf;
   const int* const supernodes = supergraphData->supernodes;
   int* supernodesid = NULL;
   STP_Bool* isSupernode = supergraphData->nodeIsSupernode;
   const STP_Bool* const solNodes = soltreeData->solNodes;
   int* const newedges = soltreeData->newedges;
   const PATH* const vnoipath = vnoiData->vnoi_path;
   const int* const vnoibase = vnoiData->vnoi_base;
   const int* const boundedges = connectData->boundedges;
   STP_Bool* const prizemark = pcmwData->prizemark;
   int* const prizemarklist = pcmwData->prizemarklist;
   const int* const graphmark = graph->mark;
   SCIP_Real mstcost = 0.0;
   int prizemarkcount = 0;
   const int nboundedges = connectData->nboundedges;
   const int nnodes = graph->knots;
   const int nsupernodes = supergraphData->nsupernodes;
   /* the (super-) vertex representing the current root-component of the Steiner tree */
   const int superroot = supernodes[nsupernodes - 1];

   assert(nboundedges > 0);
   assert(superroot >= 0);
   assert(!supergraphData->mst);

   SCIP_CALL( SCIPallocBufferArray(scip, &supernodesid, nnodes) );

   /* create a supergraph, having the endpoints of the key-paths incident to the current crucial node as (super-) vertices */
   SCIP_CALL( graph_init(scip, &supergraph, nsupernodes, nboundedges * 2, 1) );
   supergraph->stp_type = STP_SPG;

#ifndef NDEBUG
   for( int k = 0; k < nnodes; k++ )
      supernodesid[k] = -1;
#endif

   for( int k = 0; k < nsupernodes; k++ )
   {
      supernodesid[supernodes[k]] = k;
      graph_knot_add(supergraph, graph->term[supernodes[k]]);
   }

   /* add edges to the supergraph */
   for( int l = 0; l < nboundedges; l++ )
   {
      SCIP_Real edgecost;
      const int edge = boundedges[l];
      int node = SCIPStpunionfindFind(uf, vnoibase[graph->tail[edge]]);
      int adjnode = SCIPStpunionfindFind(uf, vnoibase[graph->head[edge]]);

      /* if node 'node' or 'adjnode' belongs to the root-component, take the (temporary) root-component identifier instead */
      node = ((isSupernode[node])? node : superroot);
      adjnode = ((isSupernode[adjnode])? adjnode : superroot);

      /* compute the cost of the boundary-path pertaining to the boundary-edge 'edge' */
      edgecost = vnoiGetBoundaryPathCost(graph, vnoiData, pcmwData, edge);
      graph_edge_add(scip, supergraph, supernodesid[node], supernodesid[adjnode], edgecost, edgecost);
   }

   /* compute a MST on the supergraph */
   SCIP_CALL( SCIPallocMemoryArray(scip, &(supergraphData->mst), nsupernodes) );
   mst = supergraphData->mst;
   SCIP_CALL( graph_path_init(scip, supergraph) );
   graph_path_exec(scip, supergraph, MST_MODE, nsupernodes - 1, supergraph->cost, mst);

   /* compute the cost of the MST */
   mstcost = 0.0;
   prizemarkcount = 0;

#ifndef NDEBUG
     if( graph_pc_isPcMw(graph)  )
        for( int k = 0; k < nnodes; k++ )
           assert(!prizemark[k]);
#endif

   /* compute the cost of the MST */
   for( int l = 0; l < nsupernodes - 1; l++ )
   {
      int edge;

      /* compute the edge in the original graph corresponding to the current MST edge */
      if( mst[l].edge % 2  == 0 )
         edge = boundedges[mst[l].edge / 2 ];
      else
         edge = flipedge(boundedges[mst[l].edge / 2 ]);

      mstcost += graph->cost[edge];
      mstcost -= getNewPrize(graph, solNodes, graphmark, edge, prizemark, prizemarklist, &prizemarkcount);

#ifdef SCIP_DEBUG
        printf("key vertex mst edge ");
        graph_edge_printInfo(graph, edge);
#endif

      assert( newedges[edge] != crucnode && newedges[flipedge(edge)] != crucnode );

      /* mark the edge (in the original graph) as visited */
      newedges[edge] = crucnode;

      /* traverse along the boundary-path belonging to the boundary-edge 'edge' */
      for( int node = graph->tail[edge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
      {
         const int e = vnoipath[node].edge;

         /* if edge 'e' and its reversed have not been visited yet */
         if( newedges[e] != crucnode && newedges[flipedge(e)] != crucnode )
         {
            newedges[e] = crucnode;
            mstcost += graph->cost[e];
            mstcost -= getNewPrize(graph, solNodes, graphmark, e, prizemark, prizemarklist, &prizemarkcount);
#ifdef SCIP_DEBUG
              printf("key vertex mst edge ");
              graph_edge_printInfo(graph, e);
#endif
         }
      }

      for( int node = graph->head[edge]; node != vnoibase[node]; node = graph->tail[vnoipath[node].edge] )
      {
         const int e = flipedge(vnoipath[node].edge);

         /* if edge 'e' and its reversed have not been visited yet */
         if( newedges[vnoipath[node].edge] != crucnode && newedges[e] != crucnode )
         {
            newedges[e] = crucnode;
            mstcost += graph->cost[e];
            mstcost -= getNewPrize(graph, solNodes, graphmark, e, prizemark, prizemarklist, &prizemarkcount);

#ifdef SCIP_DEBUG
              printf("key vertex mst edge ");
              graph_edge_printInfo(graph, e);
#endif
         }
      }
   }

   for( int pi = 0; pi < prizemarkcount; pi++ )
   {
      assert(graph_pc_isPcMw(graph));
      prizemark[prizemarklist[pi]] = FALSE;
   }

   supergraphData->mstcost = mstcost;

   SCIPfreeBufferArray(scip, &supernodesid);
   graph_path_exit(scip, supergraph);
   graph_free(scip, &supergraph, TRUE);

   return SCIP_OKAY;
}


/** preprocessing step for Voronoi repair */
static
void getKeyPathsStar(
   int                   keyvertex,          /**< key vertex to start from */
   const GRAPH*          graph,              /**< graph data structure */
   const CONN*           connectData,        /**< data */
   const SOLTREE*        soltreeData,        /**< solution tree data */
   KPATHS*               keypathsData,       /**< key paths */
   SGRAPH*               supergraphData,     /**< super-graph*/
   SCIP_Bool*            success             /**< success? */
)
{
   int* const kpnodes = keypathsData->kpnodes;
   int* const kpedges = keypathsData->kpedges;
   const int* const solEdges = soltreeData->solEdges;
   int* const supernodes = supergraphData->supernodes;
   STP_Bool* isSupernode = supergraphData->nodeIsSupernode;
   const STP_Bool* const solNodes = soltreeData->solNodes;
   const STP_Bool* const pinned = soltreeData->nodeIsPinned;
   int edge2root = UNKNOWN;
   int nkpnodes = 0;
   int nkpedges = 0;
   int nsupernodes = 0;

   assert(!pinned[keyvertex] && !Is_term(graph->term[keyvertex]) && nodeIsCrucial(graph, solEdges, keyvertex));

   keypathsData->kpcost = 0.0;
   keypathsData->rootpathstart = -1;
   keypathsData->nkpedges = -1;
   keypathsData->nkpnodes = -1;
   supergraphData->nsupernodes = -1;
   *success = TRUE;

   /* find all key-paths starting in node 'keyvertex' */
   for( int edge = graph->outbeg[keyvertex]; edge != EAT_LAST; edge = graph->oeat[edge] )
   {
      /* check whether the outgoing edge is in the ST */
      if( (solEdges[edge] == CONNECT && solNodes[graph->head[edge]])
          || (solEdges[flipedge(edge)] == CONNECT && solNodes[graph->tail[edge]]) )
      {
         keypathsData->kpcost += graph->cost[edge];

#ifdef SCIP_DEBUG
         printf("key vertex start edge ");
         graph_edge_printInfo(graph, edge);
#endif

         /* check whether the current edge leads to the ST root */
         if( solEdges[flipedge(edge)] == CONNECT )
         {
            edge2root = flipedge(edge);
            kpedges[nkpedges++] = edge2root;
            assert( edge == soltreeData->linkcutNodes[keyvertex].edge );
         }
         else
         {
            int adjnode = graph->head[edge];
            int e = edge;

            assert(solEdges[flipedge(edge)] == UNKNOWN);

            kpedges[nkpedges++] = e;

            /* move along the key-path until its end (i.e. a crucial or pinned node) is reached */
            while( !pinned[adjnode] && !nodeIsCrucial(graph, solEdges, adjnode) && solNodes[adjnode] )
            {
               /* update the union-find data structure */
               SCIPStpunionfindUnion(connectData->uf, keyvertex, adjnode, FALSE);

               kpnodes[nkpnodes++] = adjnode;

               for( e = graph->outbeg[adjnode]; e != EAT_LAST; e = graph->oeat[e] )
               {
                  if( solEdges[e] == CONNECT )
                  {
                     keypathsData->kpcost += graph->cost[e];
                     kpedges[nkpedges++] = e;
#ifdef SCIP_DEBUG
                     printf("key vertex edge ");
                     graph_edge_printInfo(graph, e);
#endif

                     break;
                  }
               }

               /* assert that each leaf of the ST is a terminal */

               if( e == EAT_LAST )
               {
                  *success = FALSE;
                  goto TERMINATE;
               }

               assert(e != EAT_LAST);
               adjnode = graph->head[e];
            }

            /* does the last node on the path belong to a removed component? */
            if( !solNodes[adjnode] )
            {
               keypathsData->kpcost -= graph->cost[e];
#ifdef SCIP_DEBUG
               printf("key vertex remove edge ");
               graph_edge_printInfo(graph, e);
#endif
               nkpedges--;
               adjnode = graph->tail[e];
               if( adjnode != keyvertex )
               {
                  supernodes[nsupernodes++] = adjnode;
                  isSupernode[adjnode] = TRUE;
               }
            }
            else
            {
               supernodes[nsupernodes++] = adjnode;
               isSupernode[adjnode] = TRUE;
            }
         }
      }
   }   /* find all (unique) key-paths starting in node 'crucnode' */

   /* traverse the key-path leading to the root-component */
   keypathsData->rootpathstart = nkpnodes;
   if( edge2root != UNKNOWN )
   {
      /* begin with the edge starting in the root-component of node 'keyvertex' */
      int tail = graph->tail[edge2root];

      while( !pinned[tail] && !nodeIsCrucial(graph, solEdges, tail) && solNodes[tail] )
      {
         int e;

         kpnodes[nkpnodes++] = tail;

         for( e = graph->inpbeg[tail]; e != EAT_LAST; e = graph->ieat[e] )
         {
            if( solEdges[e] > -1 )
            {
               assert(solNodes[graph->tail[e]]);
               keypathsData->kpcost += graph->cost[e];
#ifdef SCIP_DEBUG
               printf("key vertex (root) edge ");
               graph_edge_printInfo(graph, e);
#endif

               kpedges[nkpedges++] = e;
               break;
            }
         }

         assert( e != EAT_LAST );
         tail = graph->tail[e];
      }

      supernodes[nsupernodes++] = tail;
   }

   /* the last of the key-path nodes to be stored is the current key-node */
   kpnodes[nkpnodes++] = keyvertex;

 TERMINATE:

   keypathsData->nkpedges = nkpedges;
   keypathsData->nkpnodes = nkpnodes;
   supergraphData->nsupernodes = nsupernodes;
}


/** preprocessing for Voronoi repair method */
static
void vnoiDataRepairPreprocess(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const KPATHS*         keypathsData,       /**< key paths */
   const CONN*           connectData,        /**< base lists */
   const PCMW*           pcmwData,           /**< data */
   VNOI*                 vnoiData,           /**< data */
   int*                  nheapelems          /**< to store */
)
{
   IDX** const blists_start = connectData->blists_start;
   PATH* const vnoipath = vnoiData->vnoi_path;
   const int* const kpnodes = keypathsData->kpnodes;
   int* const vnoibase = vnoiData->vnoi_base;
   int* const state = vnoiData->vnoi_nodestate;
   const int* const graphmark = graph->mark;
   const int nkpnodes = keypathsData->nkpnodes;
   int count = 0;

   assert(nheapelems);

   for( int k = 0; k < nkpnodes; k++ )
   {
      IDX* blists_curr = blists_start[kpnodes[k]];
      assert( blists_curr != NULL );

      while( blists_curr != NULL )
      {
         const int node = blists_curr->index;

         /* iterate through all outgoing edges of 'node' */
         for( int edge = graph->inpbeg[node]; edge != EAT_LAST; edge = graph->ieat[edge] )
         {
            const int adjnode = graph->tail[edge];

            /* check whether the adjacent node is not in C and allows a better Voronoi assignment of the current node */
            if( state[adjnode] == CONNECT && SCIPisGT(scip, vnoipath[node].dist, vnoipath[adjnode].dist + graph->cost[edge])
               && graphmark[vnoibase[adjnode]] && graphmark[adjnode] )
            {
               vnoipath[node].dist = vnoipath[adjnode].dist + graph->cost[edge];
               vnoibase[node] = vnoibase[adjnode];
               vnoipath[node].edge = edge;
            }
         }

         if( vnoibase[node] != UNKNOWN )
         {
            heap_add(graph->path_heap, state, &count, node, vnoipath);
         }

         blists_curr = blists_curr->parent;
      }
   }

   assert(nkpnodes == 0 || count > 0);

   *nheapelems = count;
}

/** restore data */
static
void vnoiDataRestore(
   const CONN*           connectData,        /**< base lists */
   const KPATHS*         keypathsData,       /**< key paths */
   VNOI*                 vnoiData            /**< data */
)
{
   IDX** blists_start = connectData->blists_start;
   PATH* vnoipath = vnoiData->vnoi_path;
   int* memvbase = vnoiData->memvbase;
   int* meminedges = vnoiData->meminedges;
   int* vnoibase = vnoiData->vnoi_base;
   const int* kpnodes = keypathsData->kpnodes;
   SCIP_Real* memvdist = vnoiData->memvdist;
   const int nkpnodes = keypathsData->nkpnodes;
   int l = 0;

   for( int k = 0; k < nkpnodes; k++ )
   {
      /* restore data of all nodes having the current (internal) key-path node as their voronoi base */
      IDX* blists_curr = blists_start[kpnodes[k]];
      while( blists_curr != NULL )
      {
         const int node = blists_curr->index;
         vnoibase[node] = memvbase[l];
         vnoipath[node].dist = memvdist[l];
         vnoipath[node].edge = meminedges[l];
         l++;
         blists_curr = blists_curr->parent;
      }
   }

   assert(l == vnoiData->nmems);
   assert(vnoiData->nkpnodes == nkpnodes);
}

/** reset data */
static
void vnoiDataReset(
   const CONN*           connectData,        /**< base lists */
   const KPATHS*         keypathsData,       /**< key paths */
   const int*            graphmark,          /**< graph mark */
   VNOI*                 vnoiData            /**< data */
)
{
   IDX** blists_start = connectData->blists_start;
   PATH* vnoipath = vnoiData->vnoi_path;
   int* memvbase = vnoiData->memvbase;
   int* meminedges = vnoiData->meminedges;
   int* state = vnoiData->vnoi_nodestate;
   int* vnoibase = vnoiData->vnoi_base;
   const int* kpnodes = keypathsData->kpnodes;
   SCIP_Real* memvdist = vnoiData->memvdist;
   const int nkpnodes = keypathsData->nkpnodes;
   int nresnodes = 0;

   /* reset all nodes (referred to as 'C') whose bases are internal nodes of the current key-paths */
   for( int k = 0; k < nkpnodes; k++ )
   {
      /* reset all nodes having the current (internal) key-path node as their Voronoi base */
      IDX* blists_curr = blists_start[kpnodes[k]];
      while( blists_curr != NULL )
      {
         const int node = blists_curr->index;

         assert(graphmark[node]);

         /* store data */
         memvbase[nresnodes] = vnoibase[node];
         memvdist[nresnodes] = vnoipath[node].dist;
         meminedges[nresnodes] = vnoipath[node].edge;
         nresnodes++;

         /* reset data */
         vnoibase[node] = UNKNOWN;
         vnoipath[node].dist = FARAWAY;
         vnoipath[node].edge = UNKNOWN;
         state[node] = UNKNOWN;
         blists_curr = blists_curr->parent;
      }
   }

   vnoiData->nmems = nresnodes;
   vnoiData->nkpnodes = nkpnodes;
}


/** perform local vertex insertion heuristic on given Steiner tree */
static
SCIP_RETCODE localVertexInsertion(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   STP_Bool*             solNodes,           /**< Steiner tree nodes */
   NODE*                 linkcutNodes,       /**< Steiner tree nodes */
   int*                  solEdges            /**< array indicating whether an arc is part of the solution (CONNECTED/UNKNOWN) */
   )
{
   int* insert = NULL;
   int* adds = NULL;
   int* cuts = NULL;
   int* cuts2 = NULL;
   int* solDegree = NULL;
   int i = 0;
   int newnode = 0;
   int newnverts = 0;
   const int nnodes = graph->knots;
   const int nedges = graph->edges;
   const int root = graph->source;
   const STP_Bool pc = graph_pc_isPc(graph);
   const STP_Bool mw = (graph->stp_type == STP_MWCSP);
   const STP_Bool mwpc = graph_pc_isPcMw(graph);
   const int probtype = graph->stp_type;

#ifndef NDEBUG
   const SCIP_Real initialobj = graph_sol_getObj(graph->cost, solEdges, 0.0, nedges);
#endif

   if( probtype != STP_SPG && probtype != STP_RSMT && probtype != STP_OARSMT && probtype != STP_GSTP && !mwpc )
   {
      SCIPdebugMessage("vertex inclusion does not work for current problem type \n");
      return SCIP_OKAY;
   }

   SCIP_CALL( SCIPallocBufferArray(scip, &insert, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &adds, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &cuts, nnodes) );

   if( mw )
   {
      SCIP_CALL( SCIPallocBufferArray(scip, &cuts2, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &solDegree, nnodes) );

      BMSclearMemoryArray(solDegree, nnodes);

      for( int e = 0; e < nedges; e++ )
      {
         if( solEdges[e] == CONNECT )
         {
            solDegree[graph->tail[e]]++;
            solDegree[graph->head[e]]++;
         }
      }
   }

   for( ;; )
   {
      SCIP_Real diff;

      /* if vertex i is not in the current ST and has at least two adjacent nodes, it might be added */
      if( !solNodes[i] && graph->grad[i] > 1 && (!mwpc || !Is_term(graph->term[i])) )
      {
         NODE* v;
         int counter;
         int lastnodeidx;
         int insertcount = 0;

         /* if an outgoing edge of vertex i points to the current ST, SCIPlinkcuttreeLink the edge to a list */
         for( int oedge = graph->outbeg[i]; oedge != EAT_LAST; oedge = graph->oeat[oedge])
            if( solNodes[graph->head[oedge]] && (!mwpc || !Is_term(graph->term[graph->head[oedge]])) )
               insert[insertcount++] = oedge;

         /* if there are less than two edges connecting node i and the current tree, continue */
         if( insertcount <= 1 )
            goto ENDOFLOOP;

         if( mw )
            SCIPlinkcuttreeInit(&linkcutNodes[i]);

         /* the node to insert */
         v = &linkcutNodes[i];

         SCIPlinkcuttreeLink(v, &linkcutNodes[graph->head[insert[0]]], insert[0]);

         lastnodeidx = graph->head[insert[0]];

         if( mw )
         {
            assert(!SCIPisPositive(scip, graph->prize[i]));

            diff = -1.0;
            assert(solDegree != NULL);
            solDegree[lastnodeidx]++;
         }
         else
            diff = graph->cost[v->edge];

         counter = 0;

         /* try to add edges between new vertex and tree */
         for( int k = 1; k < insertcount; k++ )
         {
            NODE* firstnode;
            int firstnodidx;
            SCIPlinkcuttreeEvert(v);

            /* next vertex in the current Steiner tree adjacent to vertex i resp. v (the one being scrutinized for possible insertion) */
            firstnodidx = graph->head[insert[k]];
            firstnode = &linkcutNodes[firstnodidx];

            if( mw )
            {
               NODE* chainfirst;
               NODE* chainlast;
               SCIP_Real minweight;

               assert(solDegree != NULL);

               minweight = SCIPlinkcuttreeFindMinChain(scip, graph->prize, graph->head, solDegree, firstnode, &chainfirst, &chainlast);

               if( SCIPisLT(scip, minweight, graph->prize[i]) )
               {
                  assert(chainfirst != NULL && chainlast != NULL);
                  for( NODE* mynode = chainfirst; mynode != chainlast; mynode = mynode->parent )
                  {
                     int mynodeidx = graph->head[mynode->edge];
                     solNodes[mynodeidx] = FALSE;
                     solDegree[mynodeidx] = 0;
                  }

                  SCIPlinkcuttreeCut(chainfirst);
                  SCIPlinkcuttreeCut(chainlast);

                  SCIPlinkcuttreeLink(v, firstnode, insert[k]);
                  solDegree[graph->head[insert[k]]]++;

                  diff = graph->prize[i] - minweight;
                  break;
               }
            }
            else
            {
               /* if there is an edge with cost greater than that of the current edge... */
               NODE* max = SCIPlinkcuttreeFindMax(scip, graph->cost, firstnode);
               if( SCIPisGT(scip, graph->cost[max->edge], graph->cost[insert[k]]) )
               {
                  diff += graph->cost[insert[k]];
                  diff -= graph->cost[max->edge];
                  cuts[counter] = max->edge;
                  SCIPlinkcuttreeCut(max);
                  SCIPlinkcuttreeLink(v, firstnode, insert[k]);
                  assert(v->edge == insert[k]);
                  adds[counter++] = v->edge;
               }
            }
         }

         if( pc && Is_pterm(graph->term[i]) )
            diff -= graph->prize[i];

         /* if the new tree is more expensive than the old one, restore the latter */
         if( mw )
         {
            if( SCIPisLT(scip, diff, 0.0) )
            {
               assert(solDegree != NULL);

               SCIPlinkcuttreeEvert(v);
               solDegree[lastnodeidx]--;
               SCIPlinkcuttreeCut(&linkcutNodes[graph->head[insert[0]]]);
            }
            else
            {
               solNodes[i] = TRUE;
               newnverts++;
            }
         }
         else
         {
            if( !SCIPisNegative(scip, diff) )
            {
               SCIPlinkcuttreeEvert(v);
               for( int k = counter - 1; k >= 0; k-- )
               {
                  SCIPlinkcuttreeCut(&linkcutNodes[graph->head[adds[k]]]);
                  SCIPlinkcuttreeEvert(&linkcutNodes[graph->tail[cuts[k]]]);
                  SCIPlinkcuttreeLink(&linkcutNodes[graph->tail[cuts[k]]], &linkcutNodes[graph->head[cuts[k]]], cuts[k]);
               }

               /* finally, cut the edge added first (if it had been cut during the insertion process, it would have been restored above) */
               SCIPlinkcuttreeEvert(v);
               SCIPlinkcuttreeCut(&linkcutNodes[graph->head[insert[0]]]);
            }
            else
            {
               SCIPlinkcuttreeEvert(&linkcutNodes[root]);
               adds[counter] = insert[0];
               newnode = i;
               solNodes[i] = TRUE;
               newnverts++;
               SCIPdebugMessage("ADDED VERTEX \n");
            }
         }
      }

      ENDOFLOOP:

      if( i < nnodes - 1 )
         i++;
      else
         i = 0;

      if( newnode == i )
         break;
   }

   /* free buffer memory */
   if( mw )
   {
      SCIPfreeBufferArray(scip, &solDegree);
      SCIPfreeBufferArray(scip, &cuts2);
   }
   SCIPfreeBufferArray(scip, &cuts);
   SCIPfreeBufferArray(scip, &adds);
   SCIPfreeBufferArray(scip, &insert);

   for( int e = 0; e < nedges; e++ )
      solEdges[e] = UNKNOWN;

   if( newnverts > 0  )
   {
      if( mwpc )
         SCIP_CALL( SCIPStpHeurTMPrunePc(scip, graph, graph->cost, solEdges, solNodes) );
      else
         SCIP_CALL( SCIPStpHeurTMPrune(scip, graph, graph->cost, 0, solEdges, solNodes) );

      for( i = 0; i < nnodes; i++ )
         SCIPlinkcuttreeInit(&linkcutNodes[i]);

      /* create a link-cut tree representing the current Steiner tree */
      for( int e = 0; e < nedges; e++ )
      {
         if( solEdges[e] == CONNECT )
         {
            assert(solNodes[graph->tail[e]]);
            assert(solNodes[graph->head[e]]);
            SCIPlinkcuttreeLink(&linkcutNodes[graph->head[e]], &linkcutNodes[graph->tail[e]], flipedge(e));
         }
      }
      SCIPlinkcuttreeEvert(&linkcutNodes[root]);
   }
   else
   {
      SCIPlinkcuttreeEvert(&linkcutNodes[root]);
      for( i = 0; i < nnodes; i++ )
      {
         if( solNodes[i] && linkcutNodes[i].edge != -1 )
            solEdges[flipedge(linkcutNodes[i].edge)] = 0;
      }
   }

#ifndef NDEBUG
   {
      const SCIP_Real newobj = graph_sol_getObj(graph->cost, solEdges, 0.0, nedges);
      SCIPdebugMessage("vertex inclusion obj before/after: %f/%f \n", initialobj, newobj);
      assert(SCIPisLE(scip, newobj, initialobj));
   }
#endif

   return SCIP_OKAY;
}


/** perform local vertex insertion heuristic on given Steiner tree */
static
SCIP_RETCODE localKeyVertexHeuristics(
   SCIP*                 scip,               /**< SCIP data structure */
   GRAPH*                graph,              /**< graph data structure */
   STP_Bool*             solNodes,           /**< Steiner tree nodes */
   NODE*                 linkcutNodes,       /**< Steiner tree nodes */
   int*                  solEdges,           /**< array indicating whether an arc is part of the solution (CONNECTED/UNKNOWN) */
   SCIP_Bool*            success             /**< solution improved? */
   )
{
   UF uf;  /* union-find */
   IDX** blists_start = NULL;  /* array [1,..,nnodes],
                         * if node i is in the current ST, blists_start[i] points to a linked list of all nodes having i as their base */
   PATH* vnoipath = NULL;
   IDX** lvledges_start = NULL;  /* horizontal edges */
   PHNODE** boundpaths = NULL;
   SCIP_Real* memvdist = NULL;
   SCIP_Real* edgecost_pc = NULL;
   SCIP_Real* prize_pc = NULL;
   int* vnoibase = NULL;
   int* kpedges = NULL;
   int* kpnodes = NULL;
   int* dfstree = NULL;
   int* newedges = NULL;
   int* memvbase = NULL;
   int* pheapsize = NULL;
   int* boundedges = NULL;
   int* meminedges = NULL;
   int* supernodes = NULL;
   int* prizemarklist = NULL;
   STP_Bool* pinned = NULL;
   STP_Bool* scanned = NULL;
   STP_Bool* supernodesmark = NULL;
   STP_Bool* prizemark = NULL;
   const int probtype = graph->stp_type;
   const int root = graph->source;
   const int nnodes = graph->knots;
   const int nedges = graph->edges;
   const STP_Bool mwpc = graph_pc_isPcMw(graph);
   SCIP_Bool solimproved = FALSE;

#ifndef NDEBUG
   const SCIP_Real initialobj = graph_sol_getObj(graph->cost, solEdges, 0.0, graph->edges);
   SCIP_Real objimprovement = 0.0;
#endif

   *success = FALSE;

   /* memory needed for both Key-Path Elimination and Exchange */
   SCIP_CALL( SCIPallocBufferArray(scip, &vnoipath, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &vnoibase, nnodes) );

   /* only needed for Key-Path Elimination */
   SCIP_CALL( SCIPallocBufferArray(scip, &newedges, nedges) );
   SCIP_CALL( SCIPallocBufferArray(scip, &lvledges_start, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &boundedges, nedges) );

   /* only needed for Key-Path Exchange */

   /* memory needed for both Key-Path Elimination and Exchange */
   if( mwpc )
   {
      SCIP_CALL(SCIPallocBufferArray(scip, &edgecost_pc, nedges));
      SCIP_CALL(SCIPallocBufferArray(scip, &prize_pc, nnodes));
      SCIP_CALL(SCIPallocBufferArray(scip, &prizemark, nnodes));
      SCIP_CALL(SCIPallocBufferArray(scip, &prizemarklist, nnodes));

      for( int k = 0; k < nnodes; k++ )
         prizemark[k] = FALSE;
   }
   SCIP_CALL( SCIPallocBufferArray(scip, &scanned, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &pheapsize, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &blists_start, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &memvbase, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &memvdist, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &meminedges, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &boundpaths, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &pinned, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &dfstree, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &supernodesmark, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &supernodes, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &kpnodes, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &kpedges, nnodes) );

   for( int k = 0; k < nnodes; k++ )
      graph->mark[k] = (graph->grad[k] > 0);

   graph->mark[root] = TRUE;

   SCIP_CALL( SCIPStpunionfindInit(scip, &uf, nnodes) );

   /* main loop */
   for( int nruns = 0, localmoves = 1; nruns < LOCAL_MAXRESTARTS && localmoves > 0; nruns++ )
   {
      VNOI vnoiData = { .vnoi_path = vnoipath, .vnoi_base = vnoibase, .memvdist = memvdist, .memvbase = memvbase,
         .meminedges = meminedges, .vnoi_nodestate = graph->path_state, .nmems = 0, .nkpnodes = -1 };
      KPATHS keypathsData = { .kpnodes = kpnodes, .kpedges = kpedges, .kpcost = 0.0, .nkpnodes = 0, .nkpedges = 0,
         .kptailnode = -1 };
      CONN connectivityData = { .blists_start = blists_start, .pheap_boundpaths = boundpaths, .lvledges_start = lvledges_start,
         .pheap_sizes = pheapsize, .uf = &uf, .boundedges = boundedges, .nboundedges = 0 };
      SOLTREE soltreeData = { .solNodes = solNodes, .linkcutNodes = linkcutNodes, .solEdges = solEdges, .nodeIsPinned = pinned,
         .nodeIsScanned = scanned, .newedges = newedges };
      SGRAPH supergraphData = { .supernodes = supernodes, .nodeIsSupernode = supernodesmark,
         .mst = NULL, .mstcost = 0.0, .nsupernodes = 0 };
      PCMW pcmwData = { .prize_biased = prize_pc, .edgecost_biased = edgecost_pc, .prizemark = prizemark,
         .prizemarklist = prizemarklist };
      int nstnodes = 0;

      localmoves = 0;

      /* find a DFS order of the ST nodes */
      dfsorder(graph, solEdges, root, &nstnodes, dfstree);

      /* initialize data structures  */
      for( int k = 0; k < nnodes; k++ )
      {
         pinned[k] = FALSE;
         scanned[k] = FALSE;
         supernodesmark[k] = FALSE;
      }

      for( int e = 0; e < nedges; e++ )
         newedges[e] = UNKNOWN;

      if( mwpc )
      {
         assert(graph->extended);
         pcmwInit(scip, graph, &soltreeData, &pcmwData);

         /* compute a Voronoi diagram with the Steiner tree nodes as bases */
         graph_voronoi(scip, graph, graph->cost, graph->cost, soltreeData.solNodes, vnoiData.vnoi_base, vnoiData.vnoi_path);
      }
      else
      {
         graph_voronoi(scip, graph, graph->cost, graph->cost, soltreeData.solNodes, vnoiData.vnoi_base, vnoiData.vnoi_path);
      }

      for( int k = 0; k < nnodes; k++ )
         assert(graph->path_state[k] == CONNECT || !graph->mark[k]);

      SCIP_CALL( connectivityDataInit(scip, graph, &vnoiData, &soltreeData, &pcmwData, &connectivityData) );

      /* henceforth, the union-find structure will be used on the Steiner tree */
      assert(uf.nElements == nnodes);
      SCIPStpunionfindClear(scip, &uf);

      /* main loop visiting all nodes of the current Steiner tree in post-order */
      for( int dfstree_pos = 0; dfstree[dfstree_pos] != root; dfstree_pos++ )
      {
         /* current crucial node */
         const int crucnode = dfstree[dfstree_pos];
         int nheapelems = -1;

         scanned[crucnode] = TRUE;

         SCIPdebugMessage("iteration %d (crucial node: %d) \n", dfstree_pos, crucnode);

         /*  has the node been temporarily removed from the ST? */
         if( !graph->mark[crucnode] )
            continue;

         /* key vertex elimination: */
         /* is node 'crucnode' a removable crucial node? (i.e. not pinned or a terminal) */
         if( !pinned[crucnode] && !Is_term(graph->term[crucnode]) && nodeIsCrucial(graph, solEdges, crucnode) )
         {
            SCIP_Bool allgood;

#ifndef NDEBUG
            for( int j = 0; j < nnodes; j++ )
               assert(graph->path_state[j] == CONNECT || !graph->mark[j]);
#endif

            getKeyPathsStar(crucnode, graph, &connectivityData, &soltreeData, &keypathsData, &supergraphData, &allgood);

            if( !allgood )
            {
               *success = FALSE;
               localmoves = 0;
               SCIPdebugMessage("terminate key vertex heuristic \n");
               goto TERMINATE;
            }

            assert(keypathsData.nkpnodes != 0); /* if there are no key-path nodes, something has gone wrong */

            /* reset all nodes (referred to as 'C' henceforth) whose bases are internal nodes of the current key-paths */
            vnoiDataReset(&connectivityData, &keypathsData, graph->mark, &vnoiData);

            SCIP_CALL( connectivityDataKeyElimUpdate(scip, graph, &vnoiData, &supergraphData, crucnode, &connectivityData) );

            /* try to connect the nodes of C (directly) to COMP(C), as a preprocessing for graph_voronoiRepair */
            vnoiDataRepairPreprocess(scip, graph, &keypathsData, &connectivityData, &pcmwData, &vnoiData, &nheapelems);

            graph_voronoiRepairMult(scip, graph, graph->cost, &nheapelems, vnoibase, connectivityData.boundedges, &(connectivityData.nboundedges),
                  supernodesmark, &uf, vnoipath);

            SCIP_CALL( supergraphComputeMst(scip, graph, &connectivityData, &soltreeData, &vnoiData, &pcmwData,
                  crucnode, &keypathsData, &supergraphData) );

            assert(crucnode == dfstree[dfstree_pos]);

            /* improving solution found? */
            if( SCIPisLT(scip, supergraphData.mstcost, keypathsData.kpcost) )
            {
               localmoves++;
               solimproved = TRUE;

               SCIPdebugMessage("found improving solution in KEY VERTEX ELIMINATION (round: %d) \n ", nruns);

               SCIP_CALL( soltreeElimKeyPathsStar( scip, graph, &connectivityData, &vnoiData, &keypathsData, &supergraphData,
                  dfstree, scanned, dfstree_pos, &soltreeData) );

#ifndef NDEBUG
               assert((keypathsData.kpcost - supergraphData.mstcost) >= 0.0);
               objimprovement += (keypathsData.kpcost - supergraphData.mstcost);
#endif
            }
            else    /* no improving solution has been found during the move */
            {
               /* meld the heap pertaining to 'crucnode' and all heaps pertaining to descendant key-paths of node 'crucnode' */
               for( int k = 0; k < keypathsData.rootpathstart; k++ )
               {
                  SCIPpairheapMeldheaps(scip, &boundpaths[crucnode], &boundpaths[kpnodes[k]], &pheapsize[crucnode], &pheapsize[kpnodes[k]]);
               }
               for( int k = 0; k < supergraphData.nsupernodes - 1; k++ )
               {
                  SCIPpairheapMeldheaps(scip, &boundpaths[crucnode], &boundpaths[supernodes[k]], &pheapsize[crucnode], &pheapsize[supernodes[k]]);
                  SCIPStpunionfindUnion(&uf, crucnode, supernodes[k], FALSE);
               }
            }

            SCIPfreeMemoryArray(scip, &(supergraphData.mst));

            /* unmark the descendant supervertices */
            for( int k = 0; k < supergraphData.nsupernodes - 1; k++ )
               supernodesmark[supernodes[k]] = FALSE;

#ifndef NDEBUG
            for( int k = 0; k < nnodes; k++ )
               assert(!supernodesmark[k]);
#endif

            /* restore the original Voronoi diagram */
            vnoiDataRestore(&connectivityData, &keypathsData, &vnoiData);
         }  /* key vertex elimination: */


         /* Key-Path Exchange:
         *  If the crucnode has just been eliminated, skip Key-Path Exchange */
         if( probtype != STP_MWCSP && graph->mark[crucnode] )
         {
            SCIP_Real edgecost = -1.0;
            int e = UNKNOWN;
            int oldedge = UNKNOWN;
            int newedge = UNKNOWN;

            assert(graph->mark[crucnode]);

            /* is crucnode not a crucial node and not a pinned vertex? */
            if( (!nodeIsCrucial(graph, solEdges, crucnode) && !pinned[crucnode]) )
               continue;

            /* gets key path from crucnode towards tree root */
            getKeyPathUpper(scip, crucnode, graph, &soltreeData, &connectivityData, &keypathsData);

#ifndef NDEBUG
            for( int k = 0; k < nnodes; k++ )
               assert(graph->path_state[k] == CONNECT || !graph->mark[k]);
#endif

            /* reset all nodes (henceforth referred to as 'C') whose bases are internal nodes of the current keypath */
            vnoiDataReset(&connectivityData, &keypathsData, graph->mark, &vnoiData);

            while( boundpaths[crucnode] != NULL )
            {
               int base;
               int node;

               SCIP_CALL( SCIPpairheapDeletemin(scip, &e, &edgecost, &boundpaths[crucnode], &(pheapsize[crucnode])) );

               assert( e != UNKNOWN );
               base = vnoibase[graph->head[e]];

               assert(graph->mark[vnoibase[graph->tail[e]]]);
               node = (base == UNKNOWN || !graph->mark[base] )? UNKNOWN : SCIPStpunionfindFind(&uf, base);

               /* does the boundary-path end in the root component? */
               if( node != UNKNOWN && node != crucnode && graph->mark[base] )
               {
                  SCIP_CALL( SCIPpairheapInsert(scip, &boundpaths[crucnode], e, edgecost, &(pheapsize[crucnode])) );
                  break;
               }
            }

            if( boundpaths[crucnode] == NULL )
               oldedge = UNKNOWN;
            else
               oldedge = e;

            /* try to connect the nodes of C (directly) to COMP(C), as a preprocessing for Voronoi-repair */
            vnoiDataRepairPreprocess(scip, graph, &keypathsData, &connectivityData, &pcmwData, &vnoiData, &nheapelems);

            newedge = UNKNOWN;

            /* if there is no key path, nothing has to be repaired */
            if( keypathsData.nkpnodes > 0 )
               graph_voronoiRepair(scip, graph, graph->cost, &nheapelems, vnoibase, vnoipath, &newedge, crucnode, &uf);
            else
               newedge = linkcutNodes[crucnode].edge;

            edgecost = getKeyPathReplaceCost(scip, graph, &vnoiData, &pcmwData, &soltreeData, edgecost, oldedge, &newedge);

            if( SCIPisLT(scip, edgecost, keypathsData.kpcost) )
            {
               localmoves++;
               solimproved = TRUE;

               SCIPdebugMessage( "ADDING NEW KEY PATH (%f )\n", edgecost - keypathsData.kpcost );
#ifndef NDEBUG
               assert((keypathsData.kpcost - edgecost) >= 0.0);
               objimprovement += (keypathsData.kpcost - edgecost);
               assert(crucnode == dfstree[dfstree_pos]);
#endif

               SCIP_CALL( soltreeExchangeKeyPath(scip, graph, &connectivityData, &vnoiData, &keypathsData,
                     dfstree, scanned, dfstree_pos, newedge, &soltreeData) );
            }

            /* restore the original Voronoi diagram */
            vnoiDataRestore(&connectivityData, &keypathsData, &vnoiData);
         }
      }


      /**********************************************************/

   TERMINATE:

      assert(uf.nElements == nnodes);
      SCIPStpunionfindClear(scip, &uf);

      /* free data structures */

      for( int k = nnodes - 1; k >= 0; k-- )
      {
         if( boundpaths[k] )
            SCIPpairheapFree(scip, &boundpaths[k]);

         for( IDX* lvledges_curr = lvledges_start[k]; lvledges_curr != NULL; lvledges_curr = lvledges_start[k] )
         {
            lvledges_start[k] = lvledges_curr->parent;
            SCIPfreeBlockMemory(scip, &lvledges_curr);
         }

         for( IDX* blists_curr = blists_start[k]; blists_curr != NULL; blists_curr = blists_start[k] )
         {
            blists_start[k] = blists_curr->parent;
            SCIPfreeBlockMemory(scip, &blists_curr);
         }
      }

      /* has there been a move during this run? */
      if( localmoves > 0 )
      {
         for( int i = 0; i < nnodes; i++ )
         {
            solNodes[i] = FALSE;
            graph->mark[i] = (graph->grad[i] > 0);
            SCIPlinkcuttreeInit(&linkcutNodes[i]);
         }

         graph->mark[root] = TRUE;

         /* create a link-cut tree representing the current Steiner tree */
         for( int e = 0; e < nedges; e++ )
         {
            assert(graph->head[e] == graph->tail[flipedge(e)]);

            /* if edge e is in the tree, so are its incident vertices */
            if( solEdges[e] != -1 )
            {
               assert(CONNECT == solEdges[e]);

               solNodes[graph->tail[e]] = TRUE;
               solNodes[graph->head[e]] = TRUE;
               SCIPlinkcuttreeLink(&linkcutNodes[graph->head[e]], &linkcutNodes[graph->tail[e]], flipedge(e));
            }
         }
         assert( linkcutNodes[root].edge == -1 );
         linkcutNodes[root].edge = -1;
      }
   } /* main loop */

   /* free data structures */
   SCIPStpunionfindFreeMembers(scip, &uf);
   SCIPfreeBufferArray(scip, &kpedges);
   SCIPfreeBufferArray(scip, &kpnodes);
   SCIPfreeBufferArray(scip, &supernodes);
   SCIPfreeBufferArray(scip, &supernodesmark);
   SCIPfreeBufferArray(scip, &dfstree);
   SCIPfreeBufferArray(scip, &pinned);
   SCIPfreeBufferArray(scip, &boundpaths);
   SCIPfreeBufferArray(scip, &meminedges);
   SCIPfreeBufferArray(scip, &memvdist);
   SCIPfreeBufferArray(scip, &memvbase);
   SCIPfreeBufferArray(scip, &blists_start);
   SCIPfreeBufferArray(scip, &pheapsize);
   SCIPfreeBufferArray(scip, &scanned);
   SCIPfreeBufferArrayNull(scip, &prizemarklist);
   SCIPfreeBufferArrayNull(scip, &prizemark);
   SCIPfreeBufferArrayNull(scip, &prize_pc);
   SCIPfreeBufferArrayNull(scip, &edgecost_pc);
   SCIPfreeBufferArray(scip, &boundedges);
   SCIPfreeBufferArray(scip, &lvledges_start);
   SCIPfreeBufferArray(scip, &newedges);
   SCIPfreeBufferArray(scip, &vnoibase);
   SCIPfreeBufferArray(scip, &vnoipath);
   /******/

   if( solimproved )
   {
      SCIP_CALL( SCIPStpHeurTMpruneEdgeSol(scip, graph, solEdges) );
      *success = TRUE;
   }

#ifndef NDEBUG
   {
      const SCIP_Real newobj = graph_sol_getObj(graph->cost, solEdges, 0.0, nedges);
      SCIPdebugMessage("key vertex heuristic obj before/after: %f/%f (improvement=%f)\n", initialobj, newobj, objimprovement);
      assert(SCIPisLE(scip, newobj + objimprovement, initialobj));
   }
#endif

   return SCIP_OKAY;
}


/*
 * Callback methods of primal heuristic
 */

/** copy method for primal heuristic plugins (called when SCIP copies plugins) */
static
SCIP_DECL_HEURCOPY(heurCopyLocal)
{  /*lint --e{715}*/
   assert(scip != NULL);
   assert(heur != NULL);
   assert(strcmp(SCIPheurGetName(heur), HEUR_NAME) == 0);

   /* call inclusion method of primal heuristic */
   SCIP_CALL( SCIPStpIncludeHeurLocal(scip) );

   return SCIP_OKAY;
}

/** destructor of primal heuristic to free user data (called when SCIP is exiting) */
static
SCIP_DECL_HEURFREE(heurFreeLocal)
{   /*lint --e{715}*/
   SCIP_HEURDATA* heurdata;

   assert(heur != NULL);
   assert(strcmp(SCIPheurGetName(heur), HEUR_NAME) == 0);
   assert(scip != NULL);

   /* free heuristic data */
   heurdata = SCIPheurGetData(heur);
   assert(heurdata != NULL);
   SCIPfreeMemory(scip, &heurdata);
   SCIPheurSetData(heur, NULL);

   return SCIP_OKAY;
}

/** solving process initialization method of primal heuristic (called when branch and bound process is about to begin) */
static
SCIP_DECL_HEURINITSOL(heurInitsolLocal)
{  /*lint --e{715}*/
   SCIP_HEURDATA* heurdata;

   assert(heur != NULL);
   assert(strcmp(SCIPheurGetName(heur), HEUR_NAME) == 0);
   assert(scip != NULL);

   /* free heuristic data */
   heurdata = SCIPheurGetData(heur);

   heurdata->nfails = 1;
   heurdata->nbestsols = DEFAULT_NBESTSOLS;

   SCIP_CALL( SCIPallocMemoryArray(scip, &(heurdata->lastsolindices), heurdata->maxnsols) );

   for( int i = 0; i < heurdata->maxnsols; i++ )
      heurdata->lastsolindices[i] = -1;

   return SCIP_OKAY;
}


/** solving process deinitialization method of primal heuristic (called before branch and bound process data is freed) */
static
SCIP_DECL_HEUREXITSOL(heurExitsolLocal)
{  /*lint --e{715}*/
   SCIP_HEURDATA* heurdata;

   assert(heur != NULL);
   assert(strcmp(SCIPheurGetName(heur), HEUR_NAME) == 0);
   assert(scip != NULL);

   /* free heuristic data */
   heurdata = SCIPheurGetData(heur);
   assert(heurdata != NULL);
   assert(heurdata->lastsolindices != NULL);
   SCIPfreeMemoryArray(scip, &(heurdata->lastsolindices));

   return SCIP_OKAY;
}


/** execution method of primal heuristic */
static
SCIP_DECL_HEUREXEC(heurExecLocal)
{  /*lint --e{715}*/
   SCIP_HEURDATA* heurdata;
   SCIP_PROBDATA* probdata;
   GRAPH* graph;                             /* graph structure */
   SCIP_SOL* newsol;                         /* new solution */
   SCIP_SOL* impsol;                         /* new improved solution */
   SCIP_SOL** sols;                          /* solutions */
   SCIP_VAR** vars;                          /* SCIP variables */
   SCIP_Real pobj;
   SCIP_Real* nval;
   SCIP_Real* xval;
   int v;
   int min;
   int nvars;
   int nsols;                                /* number of all solutions found so far */
   int nedges;
   int* results;
   int* lastsolindices;
   SCIP_Bool feasible;

   assert(heur != NULL);
   assert(scip != NULL);
   assert(strcmp(SCIPheurGetName(heur), HEUR_NAME) == 0);
   assert(result != NULL);

   /* get heuristic data */
   heurdata = SCIPheurGetData(heur);
   assert(heurdata != NULL);
   lastsolindices = heurdata->lastsolindices;
   assert(lastsolindices != NULL);

   probdata = SCIPgetProbData(scip);
   assert(probdata != NULL);

   graph = SCIPprobdataGetGraph(probdata);
   assert(graph != NULL);

   *result = SCIP_DIDNOTRUN;

   /* the local heuristics may not work correctly for several problem variants*/
   if( graph->stp_type != STP_SPG && graph->stp_type != STP_RSMT && graph->stp_type != STP_OARSMT &&
      graph->stp_type != STP_PCSPG && graph->stp_type != STP_RPCSPG && graph->stp_type != STP_GSTP
      && graph->stp_type != STP_MWCSP )
      return SCIP_OKAY;

   /* don't run local in a Subscip */
   if( SCIPgetSubscipDepth(scip) > 0 )
      return SCIP_OKAY;

   /* no solution available? */
   if( SCIPgetBestSol(scip) == NULL )
      return SCIP_OKAY;

   sols = SCIPgetSols(scip);
   nsols = SCIPgetNSols(scip);
   nedges = graph->edges;

   assert(heurdata->maxnsols >= 0);

   min = MIN(heurdata->maxnsols, nsols);

   /* only process each solution once */
   for( v = 0; v < min; v++ )
   {
      if( SCIPsolGetIndex(sols[v]) != lastsolindices[v] )
      {
         /* shift all solution indices right of the new solution index */
         for( int i = min - 1; i >= v + 1; i-- )
            lastsolindices[i] = lastsolindices[i - 1];
         break;
      }
   }

   /* no new solution available? */
   if( v == min )
      return SCIP_OKAY;

   newsol = sols[v];
   lastsolindices[v] = SCIPsolGetIndex(newsol);

   /* solution not good enough? */
   if( (v > heurdata->nbestsols && !(heurdata->maxfreq)) && graph->stp_type != STP_MWCSP )
      return SCIP_OKAY;

   /* has the new solution been found by this very heuristic? */
   if( SCIPsolGetHeur(newsol) == heur )
      return SCIP_OKAY;

   *result = SCIP_DIDNOTFIND;

   vars = SCIPprobdataGetVars(scip);
   nvars = SCIPprobdataGetNVars(scip);
   xval = SCIPprobdataGetXval(scip, newsol);

   if( vars == NULL )
      return SCIP_OKAY;

   assert(vars != NULL);
   assert(xval != NULL);

   /* allocate memory */
   SCIP_CALL( SCIPallocBufferArray(scip, &results, nedges) );
   SCIP_CALL( SCIPallocBufferArray(scip, &nval, nvars) );

   /* set solution array */
   for( int e = 0; e < nedges; e++ )
   {
      if( SCIPisEQ(scip, xval[e], 1.0) )
         results[e] = CONNECT;
      else
         results[e] = UNKNOWN;
   }

   if( !graph_sol_valid(scip, graph, results) )
   {
      SCIPfreeBufferArray(scip, &nval);
      SCIPfreeBufferArray(scip, &results);
      return SCIP_OKAY;
   }

   /* pruning necessary? */
   if( SCIPsolGetHeur(newsol) == NULL ||
      !(strcmp(SCIPheurGetName(SCIPsolGetHeur(newsol)), "rec") == 0 ||
         strcmp(SCIPheurGetName(SCIPsolGetHeur(newsol)), "TM") == 0) )
   {
      const int nnodes = graph->knots;
      STP_Bool* steinertree;
      SCIP_CALL( SCIPallocBufferArray(scip, &steinertree, nnodes) );
      assert(graph_sol_valid(scip, graph, results));

      graph_sol_setVertexFromEdge(graph, results, steinertree);

      for( int e = 0; e < nedges; e++ )
         results[e] = UNKNOWN;

      if( graph_pc_isPcMw(graph) )
         SCIP_CALL( SCIPStpHeurTMPrunePc(scip, graph, graph->cost, results, steinertree) );
      else
         SCIP_CALL( SCIPStpHeurTMPrune(scip, graph, graph->cost, 0, results, steinertree) );

      SCIPfreeBufferArray(scip, &steinertree);
   }

   /* execute local heuristics */
   SCIP_CALL( SCIPStpHeurLocalRun(scip, graph, results) );

#if 0
   if( graph_pc_isPcMw(graph) )
      SCIP_CALL( SCIPStpHeurLocalExtendPcMwImp(scip, graph, results) );
#endif

   assert(nvars == nedges);

   /* can we connect the network */
   for( v = 0; v < nvars; v++ )
      nval[v] = (results[v] == CONNECT) ? 1.0 : 0.0;

   SCIP_CALL( SCIPStpValidateSol(scip, graph, nval, &feasible) );

   /* solution feasible? */
   if( feasible )
   {
      assert(nedges == nvars);

      pobj = 0.0;

      for( v = 0; v < nedges; v++ )
         pobj += graph->cost[v] * nval[v];

      /* has solution been improved? */
      if( SCIPisGT(scip, SCIPgetSolOrigObj(scip, newsol) - SCIPprobdataGetOffset(scip), pobj) )
      {
         SCIP_SOL* bestsol;
         SCIP_Bool success;

         bestsol = sols[0];
         impsol = NULL;
         SCIP_CALL( SCIPprobdataAddNewSol(scip, nval, impsol, heur, &success) );

         if( success )
         {
            *result = SCIP_FOUNDSOL;

            if( heurdata->nbestsols < heurdata->maxnsols && SCIPisGT(scip, SCIPgetSolOrigObj(scip, bestsol) - SCIPprobdataGetOffset(scip), pobj) )
            {
               heurdata->nfails = 0;
               heurdata->nbestsols++;
            }
            SCIPdebugMessage("success in local: old: %f new: %f \n", (SCIPgetSolOrigObj(scip, bestsol) - SCIPprobdataGetOffset(scip)), pobj);
         }
      }
   }

   if( *result != SCIP_FOUNDSOL )
   {
      heurdata->nfails++;
      if( heurdata->nbestsols > DEFAULT_MINNBESTSOLS && heurdata->nfails > 1 && graph->stp_type != STP_MWCSP )
         heurdata->nbestsols--;

      SCIPdebugMessage("fail! %d \n", heurdata->nbestsols);
   }

   SCIPfreeBufferArray(scip, &nval);
   SCIPfreeBufferArray(scip, &results);

   return SCIP_OKAY;
}

/*
 * primal heuristic specific interface methods
 */



/** perform local heuristics on a given Steiner tree todo delete cost parameter */
SCIP_RETCODE SCIPStpHeurLocalRun(
   SCIP*                 scip,               /**< SCIP data structure */
   GRAPH*                graph,              /**< graph data structure */
   int*                  solEdges            /**< array indicating whether an arc is part of the solution (CONNECTED/UNKNOWN) */
   )
{
   NODE* linkcutNodes;
   const int root = graph->source;
   const int nnodes = graph->knots;
   const int probtype = graph->stp_type;
   STP_Bool* solNodes;
   const STP_Bool mw = (probtype == STP_MWCSP);
   const STP_Bool mwpc = graph_pc_isPcMw(graph);
   SCIP_Bool success = FALSE;
#ifndef NDEBUG
   const SCIP_Real initialobj = graph_sol_getObj(graph->cost, solEdges, 0.0, graph->edges);
#endif

   assert(graph && solEdges);
   assert(graph_valid(scip, graph));

   if( graph->grad[root] == 0 || graph->terms == 1 )
      return SCIP_OKAY;

   if( mwpc )
   {
      assert(graph->extended);

      if( solIsTrivialPcMw(graph, solEdges) )
         return SCIP_OKAY;
   }

   SCIP_CALL( SCIPallocBufferArray(scip, &linkcutNodes, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &solNodes, nnodes) );

   if( mwpc )
      SCIP_CALL( SCIPStpHeurLocalExtendPcMw(scip, graph, graph->cost, solEdges, solNodes) );

   markSolTreeNodes(graph, solEdges, linkcutNodes, solNodes);

   assert(linkcutNodes[root].edge == -1);

   /* Call first major local heuristic */
   SCIP_CALL( localVertexInsertion(scip, graph, solNodes, linkcutNodes, solEdges) );

   assert(graph_sol_valid(scip, graph, solEdges));

   /* run Key-Vertex Elimination & Key-Path Exchange heuristics? */
   if( !mw )
   {
      SCIP_CALL( localKeyVertexHeuristics(scip, graph, solNodes, linkcutNodes, solEdges, &success) );
   }

   if( success )
   {
      int todo; // activate later and also make other changes...such as randomization, new root, more rounds for local
#if 0
      SCIP_CALL( localVertexInsertion(scip, graph, solNodes, linkcutNodes, solEdges) );
#endif
   }

#ifndef NDEBUG
   {
      const SCIP_Real newobj = graph_sol_getObj(graph->cost, solEdges, 0.0, graph->edges);
      assert(SCIPisLE(scip, newobj, initialobj));
      assert(graph_sol_valid(scip, graph, solEdges));
   }
#endif

   SCIPfreeBufferArray(scip, &solNodes);
   SCIPfreeBufferArray(scip, &linkcutNodes);

   return SCIP_OKAY;
}

/** Implication based local heuristic for (R)PC and MW */
SCIP_RETCODE SCIPStpHeurLocalExtendPcMwImp(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   int*                  result              /**< array indicating whether an arc is part of the solution (CONNECTED/UNKNOWN) */
)
{
   const int* starts = SCIPStpGetPcImplStarts(scip);
   const int* verts = SCIPStpGetPcImplVerts(scip);

   assert(graph_pc_isPcMw(graph));

   if( starts != NULL )
   {
      const int nnodes = graph->knots;
      STP_Bool* stvertex;
      int nfound = 0;
      int ptermcount = 0;

      assert(graph->extended);
      assert(verts != NULL);

      SCIPallocBufferArray(scip, &stvertex, nnodes);

      graph_sol_setVertexFromEdge(graph, result, stvertex);

      for( int i = 0; i < nnodes; i++ )
      {
         if( !Is_pterm(graph->term[i]) )
            continue;

         assert(!graph_pc_knotIsFixedTerm(graph, i));

         ptermcount++;

         if( stvertex[i] )
            continue;

         for( int j = starts[ptermcount - 1]; j < starts[ptermcount]; j++ )
         {
            const int vert = verts[j];
            if( stvertex[vert] )
            {
               /* now connect the vertex */

               graph_knot_printInfo(graph, i);
               nfound++;
               break;
            }
         }
      }

      assert(ptermcount == graph_pc_nPotentialTerms(graph));

      if( nfound > 0 )
      {
         printf("nfound: %d \n\n\n", nfound);
         /* todo prune! */
         //return SCIP_ERROR;
      }
      else
         printf("none %d \n", 0);

      SCIPfreeBufferArray(scip, &stvertex);
   }
   return SCIP_OKAY;
}

/** Greedy Extension local heuristic for (R)PC and MW */
SCIP_RETCODE SCIPStpHeurLocalExtendPcMw(
   SCIP*                 scip,               /**< SCIP data structure */
   GRAPH*                graph,              /**< graph data structure */
   const SCIP_Real*      cost,               /**< edge cost array */
   int*                  stedge,             /**< initialized array to indicate whether an edge is part of the Steiner tree */
   STP_Bool*             stvertex            /**< uninitialized array to indicate whether a vertex is part of the Steiner tree */
   )
{
   GNODE candidates[MAX(GREEDY_EXTENSIONS, GREEDY_EXTENSIONS_MW)];
   int candidatesup[MAX(GREEDY_EXTENSIONS, GREEDY_EXTENSIONS_MW)];

   PATH* path;
   PATH* orgpath;
   SCIP_PQUEUE* pqueue;
   SCIP_Real bestsolval;

   int nextensions;
   const int greedyextensions = (graph->stp_type == STP_MWCSP) ? GREEDY_EXTENSIONS_MW : GREEDY_EXTENSIONS;
   const int nedges = graph->edges;
   const int nnodes = graph->knots;
   const int root = graph->source;
   STP_Bool* stvertextmp;
   SCIP_Bool extensions = FALSE;

#ifndef NDEBUG
   const SCIP_Real initialobj = graph_sol_getObj(graph->cost, stedge, 0.0, nedges);
#endif

#ifdef NEW
   SCIP_Real* costbiased;
   SCIP_Real* prizebiased;
   SCIP_CALL( SCIPallocBufferArray(scip, &costbiased, graph->edges) );
   SCIP_CALL( SCIPallocBufferArray(scip, &prizebiased, graph->knots) );
#endif

   assert(scip != NULL);
   assert(graph != NULL);
   assert(stedge != NULL);
   assert(cost != NULL);
   assert(stvertex != NULL);
   assert(graph->extended);

   graph_pc_2transcheck(graph);
   SCIP_CALL( SCIPallocBufferArray(scip, &stvertextmp, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &orgpath, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &path, nnodes) );

   /* initialize solution vertex array with FALSE */
   BMSclearMemoryArray(stvertex, nnodes);

   stvertex[root] = TRUE;

   for( int j = 0; j < nnodes; j++ )
      path[j].edge = UNKNOWN;

   for( int e = 0; e < nedges; e++ )
      if( stedge[e] == CONNECT )
      {
         path[graph->head[e]].edge = e;
         stvertex[graph->head[e]] = TRUE;
      }

   for( int e = 0; e < nedges; e++ )
      if( stedge[e] == CONNECT )
         assert(stvertex[graph->tail[e]]);

#ifdef NEW
   graph_pc_getBiased(scip, graph, TRUE, costbiased, prizebiased);
   graph_path_st_pcmw_extendBiased(scip, graph, costbiased, prizebiased, path, stvertex, &extensions);
#else
   graph_path_st_pcmw_extend(scip, graph, cost, FALSE, path, stvertex, &extensions);
#endif

   BMScopyMemoryArray(orgpath, path, nnodes);

   /*** compute solution value and save greedyextensions many best unconnected nodes  ***/

   SCIP_CALL( SCIPpqueueCreate(&pqueue, greedyextensions, 2.0, GNODECmpByDist) );

   assert(orgpath[root].edge == UNKNOWN);

   bestsolval = 0.0;
   nextensions = 0;
   for( int i = 0; i < nnodes; i++ )
   {
      if( graph->grad[i] == 0 || root == i )
         continue;

      if( Is_term(graph->term[i]) && !graph_pc_knotIsFixedTerm(graph, i) )
         continue;

      if( stvertex[i] )
      {
         assert(orgpath[i].edge >= 0);

         bestsolval += graph->cost[orgpath[i].edge];

         if( Is_pterm(graph->term[i]) )
            bestsolval -= graph->prize[i];
      }
      else if( orgpath[i].edge != UNKNOWN && Is_pterm(graph->term[i]) )
      {
         SCIP_CALL( addToCandidates(scip, graph, path, i, greedyextensions, &nextensions, candidates, pqueue) );
      }
   }

   for( int restartcount = 0; restartcount < GREEDY_MAXRESTARTS && !graph_pc_isRootedPcMw(graph); restartcount++ )
   {
      int l = 0;
      SCIP_Bool extensionstmp = FALSE;
      int extcount = nextensions;

      /* write extension candidates into array, from max to min */
      while( SCIPpqueueNElems(pqueue) > 0 )
      {
         GNODE* min = (GNODE*) SCIPpqueueRemove(pqueue);
         assert(extcount > 0);
         candidatesup[--extcount] = min->number;
      }
      assert(extcount == 0);

      /* iteratively insert new subpaths and try to improve solution */
      for( ; l < nextensions; l++ )
      {
         const int extensioncand = candidatesup[l];
         if( !stvertex[extensioncand] )
         {
            SCIP_Real newsolval = 0.0;
            int k = extensioncand;

            BMScopyMemoryArray(stvertextmp, stvertex, nnodes);
            BMScopyMemoryArray(path, orgpath, nnodes);

            /* add new extension */
            while( !stvertextmp[k] )
            {
               stvertextmp[k] = TRUE;
               assert(orgpath[k].edge != UNKNOWN);
               k = graph->tail[orgpath[k].edge];
               assert(k != extensioncand);
            }
#ifdef NEW
            assert(graph_sol_valid(scip, graph, stedge));
            graph_path_st_pcmw_extendBiased(scip, graph, costbiased, prizebiased, path, stvertextmp, &extensionstmp);

#else
            graph_path_st_pcmw_extend(scip, graph, cost, TRUE, path, stvertextmp, &extensionstmp);
#endif

            for( int j = 0; j < nnodes; j++ )
            {
               if( graph->grad[j] == 0 || root == j )
                  continue;

               if( Is_term(graph->term[j]) && !graph_pc_knotIsFixedTerm(graph, j) )
                  continue;

               if( stvertextmp[j] )
               {
                  assert(path[j].edge >= 0);

                  newsolval += graph->cost[path[j].edge];

                  if( Is_pterm(graph->term[j]) )
                     newsolval -= graph->prize[j];
               }
            }

            /* new solution value better than old one? */
            if( SCIPisLT(scip, newsolval, bestsolval) )
            {
               extensions = TRUE;
               bestsolval = newsolval;
               BMScopyMemoryArray(stvertex, stvertextmp, nnodes);
               BMScopyMemoryArray(orgpath, path, nnodes);

               /* save greedyextensions many best unconnected nodes  */
               nextensions = 0;

               for( int j = 0; j < nnodes; j++ )
                  if( !stvertex[j] && Is_pterm(graph->term[j]) && path[j].edge != UNKNOWN )
                     SCIP_CALL( addToCandidates(scip, graph, path, j, greedyextensions, &nextensions, candidates, pqueue) );

               break;
            } /* if new solution value better than old one? */
         } /* if !stvertex[i] */
      } /* for l < nextension */

      /* no more extensions performed? */
      if( l == nextensions )
         break;
   } /* main loop */

   /* have vertices been added? */
   if( extensions )
   {
      for( int e = 0; e < nedges; e++ )
         stedge[e] = UNKNOWN;
      SCIP_CALL( SCIPStpHeurTMPrunePc(scip, graph, graph->cost, stedge, stvertex) );
   }

   SCIPpqueueFree(&pqueue);
   SCIPfreeBufferArray(scip, &path);
   SCIPfreeBufferArray(scip, &orgpath);
   SCIPfreeBufferArray(scip, &stvertextmp);

#ifdef NEW
   SCIPfreeBufferArray(scip, &prizebiased);
   SCIPfreeBufferArray(scip, &costbiased);
#endif

#ifndef NDEBUG
   assert(SCIPisLE(scip, graph_sol_getObj(graph->cost, stedge, 0.0, nedges), initialobj));
#endif

   return SCIP_OKAY;
}

/** Greedy Extension local heuristic for (R)PC and MW */
SCIP_RETCODE SCIPStpHeurLocalExtendPcMwOut(
   SCIP*                 scip,               /**< SCIP data structure */
   GRAPH*                graph,              /**< graph data structure */
   int*                  stedge,             /**< initialized array to indicate whether an edge is part of the Steiner tree */
   STP_Bool*             stvertex            /**< uninitialized array to indicate whether a vertex is part of the Steiner tree */
   )
{
   int candidates[GREEDY_EXTENSIONS];
   int ncandidates;
   DHEAP* dheap;
   STP_Bool* stvertextmp;
   SCIP_Real* dist;
   int* pred;
   const int nedges = graph->edges;
   const int nnodes = graph->knots;
   SCIP_Bool extensions = FALSE;
   int maxnode;
   const SCIP_Bool isexended = graph->extended;

#ifndef NDEBUG
   const SCIP_Real initialobj = graph_sol_getObj(graph->cost, stedge, 0.0, nedges);
#endif

   assert(scip && graph && stedge && stvertex);

   graph_pc_2orgcheck(graph);

   graph_sol_setVertexFromEdge(graph, stedge, stvertex);

   /* compute candidates for extension */

   maxnode = -1;
   ncandidates = 0;

   for( int k = 0; k < nnodes; k++ )
      if( graph->mark[k] && !stvertex[k] && Is_term(graph->term[k]) && !graph_pc_termIsNonLeaf(graph, k) )
      {
         assert(graph->mark[k]);

         if( maxnode == -1 || graph->prize[k] > graph->prize[maxnode] )
            maxnode = k;
      }

   if( maxnode != -1 )
   {
      SCIP_RANDNUMGEN* randnumgen;
      int shift;

      SCIP_CALL( SCIPcreateRandom(scip, &randnumgen, 1, TRUE) );

      SCIP_CALL( SCIPallocBufferArray(scip, &dist, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &pred, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &stvertextmp, nnodes) );

      graph_heap_create(scip, nnodes, NULL, NULL, &dheap);
      graph_init_csr(scip, graph);

      shift = SCIPrandomGetInt(randnumgen, 0, nnodes - 1);
      ncandidates = 1;
      candidates[0] = maxnode;

      for( int k = 0; k < nnodes && ncandidates < GREEDY_EXTENSIONS; k++ )
      {
         const int node = (k + shift) % nnodes;
         if( graph->mark[k] && !stvertex[node] && Is_term(graph->term[node])
            && !graph_pc_termIsNonLeaf(graph, node) && node != maxnode )
         {
            assert(graph->mark[node]);
            candidates[ncandidates++] = node;
         }
      }

      SCIPfreeRandom(scip, &randnumgen);
   }

   /* main loop */
   for( int k = 0; k < ncandidates; k++ )
   {
      const int cand = candidates[k];
      SCIP_Bool success = FALSE;

      if( stvertex[cand] )
      {
         assert(k > 0);
         continue;
      }

      graph_path_st_pcmw_extendOut(scip, graph, cand, stvertex, dist, pred, stvertextmp, dheap, &success);

      if( success )
         extensions = TRUE;
   }

   /* have vertices been added? */
   if( extensions )
   {
      graph_pc_2trans(graph);

      for( int e = 0; e < nedges; e++ )
         stedge[e] = UNKNOWN;
      SCIP_CALL( SCIPStpHeurTMPrunePc(scip, graph, graph->cost, stedge, stvertex) );
   }

   if( maxnode != -1 )
   {
      graph_heap_free(scip, TRUE, TRUE, &dheap);
      graph_free_csr(scip, graph);

      SCIPfreeBufferArray(scip, &stvertextmp);
      SCIPfreeBufferArray(scip, &pred);
      SCIPfreeBufferArray(scip, &dist);
   }

#ifndef NDEBUG
   assert(SCIPisLE(scip, graph_sol_getObj(graph->cost, stedge, 0.0, nedges), initialobj));
#endif

   if( isexended && !graph->extended )
      graph_pc_2trans(graph);

   if( !isexended && graph->extended )
      graph_pc_2org(graph);

   return SCIP_OKAY;
}


/** creates the local primal heuristic and includes it in SCIP */
SCIP_RETCODE SCIPStpIncludeHeurLocal(
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_HEURDATA* heurdata;
   SCIP_HEUR* heur;

   /* create Local primal heuristic data */
   SCIP_CALL( SCIPallocMemory(scip, &heurdata) );

   /* include primal heuristic */
   SCIP_CALL( SCIPincludeHeurBasic(scip, &heur,
         HEUR_NAME, HEUR_DESC, HEUR_DISPCHAR, HEUR_PRIORITY, HEUR_FREQ, HEUR_FREQOFS,
         HEUR_MAXDEPTH, HEUR_TIMING, HEUR_USESSUBSCIP, heurExecLocal, heurdata) );

   assert(heur != NULL);

   /* set non-NULL pointers to callback methods */
   SCIP_CALL( SCIPsetHeurCopy(scip, heur, heurCopyLocal) );
   SCIP_CALL( SCIPsetHeurFree(scip, heur, heurFreeLocal) );
   SCIP_CALL( SCIPsetHeurInitsol(scip, heur, heurInitsolLocal) );
   SCIP_CALL( SCIPsetHeurExitsol(scip, heur, heurExitsolLocal) );

   /* add local primal heuristic parameters */
   SCIP_CALL( SCIPaddBoolParam(scip, "stp/duringroot",
         "should the heuristic be called during the root node?",
         &heurdata->duringroot, FALSE, DEFAULT_DURINGROOT, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/"HEUR_NAME"/maxfreq",
         "should the heuristic be executed at maximum frequeny?",
         &heurdata->maxfreq, FALSE, DEFAULT_MAXFREQLOC, NULL, NULL) );

   SCIP_CALL( SCIPaddIntParam(scip, "heuristics/"HEUR_NAME"/maxnsols",
         "maximum number of best solutions to improve",
         &heurdata->maxnsols, FALSE, DEFAULT_MAXNBESTSOLS, 1, 50, NULL, NULL) );

   return SCIP_OKAY;
}
