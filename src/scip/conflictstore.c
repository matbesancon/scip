/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2015 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   conflictstore.c
 * @brief  methods for storing conflicts
 * @author Jakob Witzig
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include <assert.h>
#include <string.h>

#include "scip/conflictstore.h"
#include "scip/cons.h"
#include "scip/event.h"
#include "scip/set.h"
#include "scip/tree.h"
#include "scip/misc.h"
#include "scip/prob.h"
#include "scip/scip.h"

#include "scip/struct_conflictstore.h"


#define DEFAULT_CONFLICTSTORE_SIZE       10000 /* default size of conflict storage */
#define DEFAULT_CONFLICTSTORE_MAXSIZE    50000 /* maximal size of conflict storage */

/* event handler properties */
#define EVENTHDLR_NAME         "ConflictStore"
#define EVENTHDLR_DESC         "Solution event handler for conflict store."


/* exec the event handler */
static
SCIP_DECL_EVENTEXEC(eventExecConflictstore)
{
   assert(eventhdlr != NULL);
   assert(eventdata != NULL);
   assert(strcmp(SCIPeventhdlrGetName(eventhdlr), EVENTHDLR_NAME) == 0);
   assert(event != NULL);
   assert(SCIPeventGetType(event) & SCIP_EVENTTYPE_BESTSOLFOUND);

   SCIP_CALL( SCIPcleanConflictStoreBoundexceeding(scip, event) );

   return SCIP_OKAY;
}

/*
 * dynamic memory arrays
 */

/** resizes cuts and score arrays to be able to store at least num entries */
static
SCIP_RETCODE conflictstoreEnsureMem(
   SCIP_CONFLICTSTORE*   conflictstore,      /**< conflict storage */
   SCIP_SET*             set,                /**< global SCIP settings */
   int                   num                 /**< minimal number of slots in array */
   )
{
   assert(conflictstore != NULL);
   assert(set != NULL);

   /* we do not allocate more memory as allowed */
   if( conflictstore->conflictsize == conflictstore->maxstoresize )
      return SCIP_OKAY;

   if( num > conflictstore->conflictsize )
   {
      int newsize;
      int i;

      /* initialize the complete data structure */
      if( conflictstore->conflictsize == 0 )
      {
         newsize = MIN(conflictstore->maxstoresize, DEFAULT_CONFLICTSTORE_SIZE);
         SCIP_CALL( SCIPqueueCreate(&conflictstore->slotqueue, newsize, 2) );
         SCIP_CALL( SCIPqueueCreate(&conflictstore->orderqueue, newsize, 2) );
         SCIP_ALLOC( BMSallocMemoryArray(&conflictstore->conflicts, newsize) );
         SCIP_ALLOC( BMSallocMemoryArray(&conflictstore->primalbounds, newsize) );
      }
      else
      {
         newsize = MIN(conflictstore->maxstoresize, conflictstore->conflictsize * 2);
         SCIP_ALLOC( BMSreallocMemoryArray(&conflictstore->conflicts, newsize) );
         SCIP_ALLOC( BMSreallocMemoryArray(&conflictstore->primalbounds, newsize) );
      }

      /* add all new slots (oldsize,...,newsize-1) with a shift of +1 to the slotqueue */
      for( i = conflictstore->conflictsize; i < newsize; i++ )
      {
         conflictstore->conflicts[i] = NULL;
         conflictstore->primalbounds[i] = -SCIPsetInfinity(set);
         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (i+1)) );
      }
      conflictstore->conflictsize = newsize;
   }
   assert(num <= conflictstore->conflictsize);

   assert(SCIPqueueNElems(conflictstore->slotqueue)+conflictstore->nconflicts == conflictstore->conflictsize );
   assert(SCIPqueueNElems(conflictstore->slotqueue)+SCIPqueueNElems(conflictstore->orderqueue) == conflictstore->conflictsize);

   return SCIP_OKAY;
}

/** remove all deleted conflicts from the storage */
static
SCIP_RETCODE cleanDeletedConflicts(
   SCIP_CONFLICTSTORE*   conflictstore,
   int*                  ndelconfs,
   BMS_BLKMEM*           blkmem,
   SCIP_SET*             set
   )
{
   int firstidx;

   assert(conflictstore != NULL);
   assert(SCIPqueueNElems(conflictstore->slotqueue)+conflictstore->nconflicts == conflictstore->conflictsize );
   assert(SCIPqueueNElems(conflictstore->slotqueue)+SCIPqueueNElems(conflictstore->orderqueue) == conflictstore->conflictsize);

   (*ndelconfs) = 0;
   firstidx = -1;

   while( firstidx != ((int) (size_t) SCIPqueueFirst(conflictstore->orderqueue)-1) )
   {
      SCIP_CONS* conflict;
      int idx;

      assert(!SCIPqueueIsEmpty(conflictstore->orderqueue));
      idx = ((int) (size_t) SCIPqueueRemove(conflictstore->orderqueue)) - 1;
      assert(idx >= 0 && idx < conflictstore->conflictsize);

      if( conflictstore->conflicts[idx] == NULL )
         continue;

      /* get the oldest conflict */
      conflict = conflictstore->conflicts[idx];

      /* check whether the constraint is already marked as deleted */
      if( SCIPconsIsDeleted(conflict) )
      {
         /* release the constraint */
         SCIP_CALL( SCIPconsRelease(&conflict, blkmem, set) );

         conflictstore->ncbconflicts -= (SCIPsetIsInfinity(set, REALABS(conflictstore->primalbounds[idx])) ? 0 : 1);

         /* clean the conflict and primal bound array */
         conflictstore->conflicts[idx] = NULL;
         conflictstore->primalbounds[idx] = -SCIPsetInfinity(set);

         /* add the id shifted by +1 to the queue of empty slots */
         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );

         ++(*ndelconfs);
      }
      else
      {
         /* remember the first conflict that is not deleted */
         if( firstidx == -1 )
            firstidx = idx;

         SCIP_CALL( SCIPqueueInsert(conflictstore->orderqueue, (void*) (size_t) (idx+1)) );
      }
   }

   SCIPdebugMessage("removed %d/%d as deleted marked conflicts.\n", *ndelconfs, conflictstore->nconflicts);

   assert(SCIPqueueNElems(conflictstore->slotqueue)+conflictstore->nconflicts-(*ndelconfs) == conflictstore->conflictsize );
   assert(SCIPqueueNElems(conflictstore->slotqueue)+SCIPqueueNElems(conflictstore->orderqueue) == conflictstore->conflictsize);

   return SCIP_OKAY;
}

/** clean up the storage */
static
SCIP_RETCODE conflictstoreCleanUpStorage(
   SCIP_CONFLICTSTORE*   conflictstore,
   BMS_BLKMEM*           blkmem,
   SCIP_SET*             set,
   SCIP_STAT*            stat,
   SCIP_PROB*            transprob
   )
{
   SCIP_CONS* conflict;
   SCIP_Real maxage;
   int idx;
   int ndelconfs;
   int ndelconfstmp;
   int nseenconfs;
   int tmpidx;
   int nimpr;

   assert(conflictstore != NULL);
   assert(blkmem != NULL);
   assert(set != NULL);
   assert(stat != NULL);
   assert(transprob != NULL);

   assert(SCIPqueueNElems(conflictstore->slotqueue)+conflictstore->nconflicts == conflictstore->conflictsize );
   assert(SCIPqueueNElems(conflictstore->slotqueue)+SCIPqueueNElems(conflictstore->orderqueue) == conflictstore->conflictsize);

   /* the storage is empty  */
   if( conflictstore->nconflicts == 0 )
   {
      assert(SCIPqueueNElems(conflictstore->slotqueue) == conflictstore->conflictsize);
      return SCIP_OKAY;
   }
   assert(conflictstore->nconflicts >= 1);

   /* increase the number of clean up */
   ++conflictstore->ncleanups;

   ndelconfs = 0;
   ndelconfstmp = 0;
   nseenconfs = 0;

   /* remove all as deleted marked conflicts */
   SCIP_CALL( cleanDeletedConflicts(conflictstore, &ndelconfstmp, blkmem, set) );
   ndelconfs += ndelconfstmp;
   ndelconfstmp = 0;

   assert(SCIPqueueNElems(conflictstore->slotqueue)+conflictstore->nconflicts-ndelconfs == conflictstore->conflictsize );
   assert(SCIPqueueNElems(conflictstore->slotqueue)+SCIPqueueNElems(conflictstore->orderqueue) == conflictstore->conflictsize);

   /* only clean up the storage if it is filled enough */
   if( conflictstore->nconflicts-ndelconfs < conflictstore->conflictsize-10*set->conf_maxconss )
      goto TERMINATE;

   /* we have deleted enough conflicts */
   if( ndelconfs >= 2*set->conf_maxconss )
      goto TERMINATE;

   /* if the storage is small and not full we will stop here */
   if( conflictstore->conflictsize <= 2000 && conflictstore->nconflicts-ndelconfs < conflictstore->conflictsize )
      goto TERMINATE;

   assert(!SCIPqueueIsEmpty(conflictstore->orderqueue));

   nimpr = 0;
   tmpidx = -1;
   maxage = -SCIPsetInfinity(set);

   /* find a conflict with a locally maximum age */
   while( nseenconfs < conflictstore->nconflicts-ndelconfs )
   {
      /* get the oldest conflict */
      assert(!SCIPqueueIsEmpty(conflictstore->orderqueue));
      idx = ((int) (size_t) SCIPqueueRemove(conflictstore->orderqueue)) - 1;
      assert(idx >= 0 && idx < conflictstore->conflictsize);

      if( conflictstore->conflicts[idx] == NULL )
      {
         /* add the id shifted by +1 to the queue of empty slots */
         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );
         continue;
      }

      /* get the oldest conflict */
      conflict = conflictstore->conflicts[idx];
      assert(!SCIPconsIsDeleted(conflict));

      ++nseenconfs;

      /* check if the age of the conflict is positive and larger than maxage; do nothing we have seen enough improvements */
      if( SCIPsetIsGT(set, SCIPconsGetAge(conflict), 0.0) && SCIPsetIsLT(set, maxage, SCIPconsGetAge(conflict))
          && nimpr < MIN(0.05*conflictstore->maxstoresize, 50) )
      {
         maxage = SCIPconsGetAge(conflict);
         tmpidx = idx;
         ++nimpr;
      }

      /* reinsert the id */
      SCIP_CALL( SCIPqueueInsert(conflictstore->orderqueue, (void*) (size_t) (idx+1)) );
   }

   /* no conflict was chosen because all conflicts have age 0 */
   assert(tmpidx >= 0 || SCIPsetIsInfinity(set, -maxage));
   assert(!SCIPqueueIsEmpty(conflictstore->orderqueue));
   maxage = tmpidx == -1 ? 0 : maxage;

   /* iterate over all conflicts and remove those with an age larger or equal the local maximum maxage */
   nseenconfs = 0;
   ndelconfstmp = 0;
   while( nseenconfs < conflictstore->nconflicts-ndelconfs )
   {
      /* get the oldest conflict */
      assert(!SCIPqueueIsEmpty(conflictstore->orderqueue));
      idx = ((int) (size_t) SCIPqueueRemove(conflictstore->orderqueue)) - 1;
      assert(idx >= 0 && idx < conflictstore->conflictsize);

      if( conflictstore->conflicts[idx] == NULL )
      {
         /* add the id shifted by +1 to the queue of empty slots */
         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );
         continue;
      }

      conflict = conflictstore->conflicts[idx];
      ++nseenconfs;
      assert(conflict != NULL);
      assert(!SCIPconsIsDeleted(conflict));

      if( SCIPsetIsLT(set, SCIPconsGetAge(conflict), maxage) )
      {
         SCIP_CALL( SCIPqueueInsert(conflictstore->orderqueue, (void*) (size_t) (idx+1)) );
         continue;
      }

      /* mark the constraint as deleted */
      SCIP_CALL( SCIPconsDelete(conflict, blkmem, set, stat, transprob) );
      SCIP_CALL( SCIPconsRelease(&conflict, blkmem, set) );

      /* clean the conflict and primal bound array */
      conflictstore->conflicts[idx] = NULL;
      conflictstore->primalbounds[idx] = -SCIPsetInfinity(set);

      /* add the id shifted by +1 to the queue of empty slots */
      SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );

      ++ndelconfstmp;
      SCIPdebugMessage("-> removed conflict at pos=%d with age=%g\n", idx, maxage);

      /* all conflicts have age 0, we delete the oldest conflicts */
      if( SCIPsetIsEQ(set, maxage, 0.0) )
      {
         assert(tmpidx == -1);
         break;
      }
   }

   assert(SCIPqueueNElems(conflictstore->orderqueue) <= conflictstore->maxstoresize);
   ndelconfs += ndelconfstmp;

  TERMINATE:
   SCIPdebugMessage("clean-up #%lld: removed %d/%d conflicts, %d depending on cutoff bound\n",
         conflictstore->ncleanups, ndelconfs, conflictstore->nconflicts, conflictstore->ncbconflicts);
   conflictstore->nconflicts -= ndelconfs;

   assert(SCIPqueueNElems(conflictstore->slotqueue)+conflictstore->nconflicts == conflictstore->conflictsize );
   assert(SCIPqueueNElems(conflictstore->slotqueue)+SCIPqueueNElems(conflictstore->orderqueue) == conflictstore->conflictsize);

   return SCIP_OKAY;
}


void printConflictDialRayStats(
   SCIP_CONFLICTSTORE*    conflictstore
   )
{
   SCIP_Real avg[4];
   int min[4];
   int max[4];
   int sum[4];
   int quant5[4];
   int quant5_i[4];
   int quant9[4];
   int quant9_i[4];
   int i;

   for( i = 0; i < 4; i++ )
   {
      avg[i] = 0.0;
      min[i] = conflictstore->maxsize+1;
      max[i] = 0;
      sum[i] = 0;
      quant5[i] = 0;
      quant5_i[i] = 0;
      quant9[i] = 0;
      quant9_i[i] = 0;
   }

   /* calculate the sum, avg, min, and max */
   for( i = 0; i < conflictstore->maxsize; i++ )
   {
      /* initial set of bound changes */
      sum[0] += conflictstore->dualrayinitsize[i];

      if( conflictstore->dualrayinitsize[i] > 0 )
      {
         if( i < min[0] )
            min[0] = i;
         if( i > max[0] )
            max[0] = i;
         avg[0] += conflictstore->dualrayinitsize[i] * (SCIP_Real)i;
      }

      /* set of bound changes after bound heurisric */
      sum[1] += conflictstore->dualraysize[i];

      if( conflictstore->dualraysize[i] > 0 )
      {
         if( i < min[1] )
            min[1] = i;
         if( i > max[1] )
            max[1] = i;
         avg[1] += conflictstore->dualraysize[i] * (SCIP_Real)i;
      }

      /* number of clauses after confict analysis (per conflict) */
      sum[2] += conflictstore->nclauses[i];

      if( conflictstore->nclauses[i] > 0 )
      {
         if( i < min[2] )
            min[2] = i;
         if( i > max[2] )
            max[2] = i;
         avg[2] += conflictstore->nclauses[i] * (SCIP_Real)i;
      }

      /* number of generated conficts per set of bound changes */
      sum[3] += conflictstore->nconflictsets[i];

      if( conflictstore->nconflictsets[i] > 0 )
      {
         if( i < min[3] )
            min[3] = i;
         if( i > max[3] )
            max[3] = i;
         avg[3] += conflictstore->nconflictsets[i] * (SCIP_Real)i;
      }
   }
   assert(sum[0] > 0);
   avg[0] /= sum[0];

   assert(sum[1] > 0);
   avg[1] /= sum[1];

   assert(sum[2] > 0);
   avg[2] /= sum[2];

   assert(sum[3] > 0);
   avg[3] /= sum[3];

   /* calculate quantiles */
   for( i = 0; i < conflictstore->maxsize; i++ )
   {
      /* initial set size */
      if( quant9[0] < 0.9*sum[0] )
      {
         quant9[0] += conflictstore->dualrayinitsize[i];
         quant9_i[0] = i;
         if( quant5[0] < 0.5*sum[0] )
         {
            quant5[0] += conflictstore->dualrayinitsize[i];
            quant5_i[0] = i;
         }
      }
      /* set size after bound heuristic */
      if( quant9[1] < 0.9*sum[1] )
      {
         quant9[1] += conflictstore->dualraysize[i];
         quant9_i[1] = i;
         if( quant5[1] < 0.5*sum[1] )
         {
            quant5[1] += conflictstore->dualraysize[i];
            quant5_i[1] = i;
         }
      }
      /* number of clauses per generated conflict */
      if( quant9[2] < 0.9*sum[2] )
      {
         quant9[2] += conflictstore->nclauses[i];
         quant9_i[2] = i;
         if( quant5[2] < 0.5*sum[2] )
         {
            quant5[2] += conflictstore->nclauses[i];
            quant5_i[2] = i;
         }
      }
      /* number of generated conflict */
      if( quant9[3] < 0.9*sum[3] )
      {
         quant9[3] += conflictstore->nconflictsets[i];
         quant9_i[3] = i;
         if( quant5[3] < 0.5*sum[3] )
         {
            quant5[3] += conflictstore->nconflictsets[i];
            quant5_i[3] = i;
         }
      }
   }

   printf("Conflict-DualRay Statistics:  %8s %8s %8s %8s %8s\n", "min", "quant5", "quant9", "max", "avg");
   printf(" initial set size :           %8d %8d %8d %8d %8.2f\n", min[0], quant5_i[0], quant9_i[0], max[0], avg[0]);
   printf(" shriked set size :           %8d %8d %8d %8d %8.2f\n", min[1], quant5_i[1], quant9_i[1], max[1], avg[1]);
   printf(" conflict length  :           %8d %8d %8d %8d %8.2f\n", min[2], quant5_i[2], quant9_i[2], max[2], avg[2]);
   printf(" conflict sets    :           %8d %8d %8d %8d %8.2f\n", min[3], quant5_i[3], quant9_i[3], max[3], avg[3]);
}


/** creates conflict storage */
SCIP_RETCODE SCIPconflictstoreCreate(
   SCIP_CONFLICTSTORE**  conflictstore,      /**< pointer to store conflict storage */
   SCIP_SET*             set                 /**< global SCIP settings */
   )
{
   assert(conflictstore != NULL);

   SCIP_ALLOC( BMSallocMemory(conflictstore) );

   (*conflictstore)->conflicts = NULL;
   (*conflictstore)->primalbounds = NULL;
   (*conflictstore)->slotqueue = NULL;
   (*conflictstore)->orderqueue = NULL;
   (*conflictstore)->avgswitchlength = 0;
   (*conflictstore)->conflictsize = 0;
   (*conflictstore)->nconflicts = 0;
   (*conflictstore)->ncbconflicts = 0;
   (*conflictstore)->nconflictsfound = 0;
   (*conflictstore)->maxstoresize = -1;
   (*conflictstore)->ncleanups = 0;
   (*conflictstore)->nswitches = 1;
   (*conflictstore)->cleanupfreq = -1;
   (*conflictstore)->lastnodenum = -1;

   // statistics, only temp
   (*conflictstore)->dualrayinitsize = NULL;
   (*conflictstore)->dualraysize = NULL;
   (*conflictstore)->nconflictsets = NULL;
   (*conflictstore)->nclauses = NULL;
   (*conflictstore)->maxsize = 0;

   /* create event handler for LP events */
   SCIP_CALL( SCIPeventhdlrCreate(&(*conflictstore)->eventhdlr, EVENTHDLR_NAME, EVENTHDLR_DESC, NULL, NULL, NULL, NULL,
         NULL, NULL, NULL, eventExecConflictstore, NULL) );
   SCIP_CALL( SCIPsetIncludeEventhdlr(set, (*conflictstore)->eventhdlr) );

   if( (*conflictstore)->eventhdlr == NULL )
   {
      SCIPerrorMessage("event handler for conflictstore not found.\n");
      return SCIP_PLUGINNOTFOUND;
   }

   /* initialize the conflict handler */
   SCIP_CALL( SCIPeventhdlrInit((*conflictstore)->eventhdlr, set) );

   return SCIP_OKAY;
}

/** frees conflict storage */
SCIP_RETCODE SCIPconflictstoreFree(
   SCIP_CONFLICTSTORE**  conflictstore,      /**< pointer to store conflict storage */
   BMS_BLKMEM*           blkmem,
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_EVENTFILTER*     eventfilter
   )
{
   SCIP_CONS* conflict;

   assert(conflictstore != NULL);
   assert(*conflictstore != NULL);

   printConflictDialRayStats(*conflictstore);

   /* free statistics */
   BMSfreeMemoryArray(&(*conflictstore)->dualrayinitsize);
   BMSfreeMemoryArray(&(*conflictstore)->dualraysize);
   BMSfreeMemoryArray(&(*conflictstore)->nclauses);
   BMSfreeMemoryArray(&(*conflictstore)->nconflictsets);

   if( (*conflictstore)->nconflictsfound > 0 && set->conf_cleanboundexeedings )
   {
      /* remove solution event from eventfilter */
      SCIP_CALL( SCIPeventfilterDel(eventfilter, blkmem, set, SCIP_EVENTTYPE_BESTSOLFOUND, (*conflictstore)->eventhdlr,
            (SCIP_EVENTDATA*)(*conflictstore), -1) );
   }

   if( (*conflictstore)->orderqueue != NULL )
   {
      assert((*conflictstore)->slotqueue != NULL);

      while( !SCIPqueueIsEmpty((*conflictstore)->orderqueue) )
      {
         int idx;

         idx = ((int) (size_t) SCIPqueueRemove((*conflictstore)->orderqueue)) - 1;
         assert(idx >= 0 && idx < (*conflictstore)->conflictsize);

         if( (*conflictstore)->conflicts[idx] == NULL )
            continue;

         /* get the conflict */
         conflict = (*conflictstore)->conflicts[idx];

         SCIP_CALL( SCIPconsRelease(&conflict, blkmem, set) );
         --(*conflictstore)->nconflicts;
      }

      /* free the queues */
      SCIPqueueFree(&(*conflictstore)->slotqueue);
      SCIPqueueFree(&(*conflictstore)->orderqueue);
   }
   assert((*conflictstore)->nconflicts == 0);

   BMSfreeMemoryArrayNull(&(*conflictstore)->conflicts);
   BMSfreeMemoryArrayNull(&(*conflictstore)->primalbounds);
   BMSfreeMemory(conflictstore);

   return SCIP_OKAY;
}

/** adds a conflict to the conflict storage */
SCIP_RETCODE SCIPconflictstoreAddConflict(
   SCIP_CONFLICTSTORE*   conflictstore,      /**< conflict storage */
   BMS_BLKMEM*           blkmem,             /**< block memory */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic SCIP statistics */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_PROB*            transprob,          /**< transformed problem */
   SCIP_EVENTFILTER*     eventfilter,        /**< eventfiler */
   SCIP_CONS*            cons,               /**< constraint representing the conflict */
   SCIP_NODE*            node,               /**< node to add conflict (or NULL if global) */
   SCIP_NODE*            validnode,          /**< node at whichaddConf the constraint is valid (or NULL) */
   SCIP_CONFTYPE         conftype,           /**< type of the conflict */
   SCIP_Bool             cutoffinvolved,     /**< is a cutoff bound invaled in this conflict */
   SCIP_Real             primalbound         /**< primal bound the conflict depend on (or -SCIPinfinity) */
   )
{
   int nconflicts;
   int idx;

   assert(conflictstore != NULL);
   assert(blkmem != NULL);
   assert(set != NULL);
   assert(stat != NULL);
   assert(tree != NULL);
   assert(transprob != NULL);
   assert(eventfilter != NULL);
   assert(cons != NULL);
   assert(node != NULL);
   assert(validnode != NULL);
   assert(set->conf_allowlocal || SCIPnodeGetDepth(validnode) == 0);
   assert(conftype != SCIP_CONFTYPE_UNKNOWN);
   assert(conftype != SCIP_CONFTYPE_BNDEXCEEDING || cutoffinvolved);
   assert(!cutoffinvolved || (cutoffinvolved && !SCIPsetIsInfinity(set, REALABS(primalbound))));

   nconflicts = conflictstore->nconflicts;

   /* calculate the maximal size of the conflict storage */
   if( conflictstore->maxstoresize == -1 )
   {
      /* the size should be dynamic wrt number of variables after presolving */
      SCIP_CALL( SCIPsetGetIntParam(set, "conflict/maxstoresize", &conflictstore->maxstoresize) );
      if( conflictstore->maxstoresize == 0 )
      {
         int nconss;
         int nvars;

         nconss = SCIPprobGetNConss(transprob);
         nvars = SCIPprobGetNVars(transprob);

         conflictstore->maxstoresize = 1000;
         conflictstore->maxstoresize += 2*nconss;

         if( nvars/2 <= 500 )
            conflictstore->maxstoresize += (int) DEFAULT_CONFLICTSTORE_MAXSIZE/100;
         else if( nvars/2 <= 5000 )
            conflictstore->maxstoresize += (int) DEFAULT_CONFLICTSTORE_MAXSIZE/10;
         else
            conflictstore->maxstoresize += DEFAULT_CONFLICTSTORE_MAXSIZE/2;

         conflictstore->maxstoresize = MIN(conflictstore->maxstoresize, DEFAULT_CONFLICTSTORE_MAXSIZE);
      }
      else if( set->conf_maxstoresize == -1 )
         conflictstore->maxstoresize = INT_MAX;
      SCIPdebugMessage("maximal size of conflict pool is %d.\n", conflictstore->maxstoresize);

      /* get the clean-up frequency */
      SCIP_CALL( SCIPsetGetIntParam(set, "conflict/cleanupfreq", &(conflictstore->cleanupfreq)) );

      if( set->conf_cleanboundexeedings )
      {
         /* add solution event to the eventfilter */
         SCIP_CALL( SCIPeventfilterAdd(eventfilter, blkmem, set, SCIP_EVENTTYPE_BESTSOLFOUND, conflictstore->eventhdlr,
               (SCIP_EVENTDATA*)conflictstore, NULL) );
      }

      if( set->conf_maxswitchinglength == 0 )
      {
         /* initialize average switching size */
         conflictstore->avgswitchlength = 0.9 * (SCIPprobGetNBinVars(transprob) + SCIPprobGetNIntVars(transprob));
      }
      else
         conflictstore->avgswitchlength = set->conf_maxswitchinglength;
      SCIPdebugMessage("max. switching length = %g%s\n", conflictstore->avgswitchlength, set->conf_maxswitchinglength == 0 ? " (dynamic)" : "");
   }
   assert(conflictstore->maxstoresize >= 1);
   assert(conflictstore->cleanupfreq >= 0);

   SCIP_CALL( conflictstoreEnsureMem(conflictstore, set, nconflicts+1) );

   /* return if the store has size zero */
   if( conflictstore->conflictsize == 0 )
   {
      assert(conflictstore->maxstoresize == 0);
      return SCIP_OKAY;
   }

   /* clean up the storage if we are at a new node or the storage is full */
   if( conflictstore->lastnodenum != SCIPnodeGetNumber(SCIPtreeGetFocusNode(tree)) || SCIPqueueIsEmpty(conflictstore->slotqueue) )
   {
      SCIP_CALL( conflictstoreCleanUpStorage(conflictstore, blkmem, set, stat, transprob) );
   }

   /* update the last seen node */
   conflictstore->lastnodenum = SCIPnodeGetNumber(SCIPtreeGetFocusNode(tree));

   /* get a free slot */
   assert(!SCIPqueueIsEmpty(conflictstore->slotqueue));
   idx = ((int) (size_t) SCIPqueueRemove(conflictstore->slotqueue)-1);
   assert(idx >= 0 && idx < conflictstore->conflictsize);
   assert(conflictstore->conflicts[idx] == NULL);
   assert(conflictstore->primalbounds[idx] == -SCIPsetInfinity(set));

   SCIPconsCapture(cons);
   conflictstore->conflicts[idx] = cons;
   conflictstore->primalbounds[idx] = primalbound;
   conflictstore->ncbconflicts += (SCIPsetIsInfinity(set, REALABS(primalbound)) ? 0 : 1);

   /* add idx shifted by +1 to the ordering queue */
   SCIP_CALL( SCIPqueueInsert(conflictstore->orderqueue, (void*) (size_t) (idx+1)) );

   ++conflictstore->nconflicts;
   ++conflictstore->nconflictsfound;

   SCIPdebugMessage("add conflict <%s> to conflict store at position %d\n", SCIPconsGetName(cons), idx);
   SCIPdebugMessage(" -> conflict type: %d, cutoff involved = %u\n", conftype, cutoffinvolved);
   if( cutoffinvolved )
      SCIPdebugMessage(" -> current primal bound: %g\n", primalbound);
   SCIPdebugMessage(" -> found at node %llu (depth: %d), valid at node %llu (depth: %d)\n", SCIPnodeGetNumber(node),
         SCIPnodeGetDepth(node), SCIPnodeGetNumber(validnode), SCIPnodeGetDepth(validnode));

   return SCIP_OKAY;
}

/** delete all conflicts arises from infeasible LP analysis */
SCIP_RETCODE SCIPconflictstoreCleanSwitching(
   SCIP_CONFLICTSTORE*   conflictstore,      /**< conflict storage */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic SCIP statistics */
   BMS_BLKMEM*           blkmem,             /**< block memory */
   SCIP_PROB*            transprob,          /**< transformed problem*/
   int                   switchinglength     /**< number of bound changes between focusnode - fork - new focusnode */
   )
{
   SCIP_CONS* conflict;
   SCIP_Real oldavg;
   int nseenconfs;
   int ndelconfs;
   int ndelconfs_del;
   int idx;

   assert(conflictstore != NULL);
   assert(set != NULL);
   assert(stat != NULL);
   assert(blkmem != NULL);
   assert(transprob != NULL);

   nseenconfs = 0;
   ndelconfs = 0;
   ndelconfs_del = 0;

   /* return if we do not want to use the storage */
   if( set->conf_maxstoresize == -1 )
      return SCIP_OKAY;

   /* clean up is disabled */
   if( !set->conf_cleanafterswitching )
      return SCIP_OKAY;

   /* in automatic mode we do not clean if the switching length is 1 or 2 */
   if( set->conf_maxswitchinglength == 0 && switchinglength <= 2 )
      return SCIP_OKAY;

   /* increase number of switches */
   ++conflictstore->nswitches;

   /* update average switches */
   if( set->conf_maxswitchinglength == 0 )
   {
      oldavg = conflictstore->avgswitchlength;
      conflictstore->avgswitchlength += (switchinglength - oldavg)/conflictstore->nswitches;
   }

   if( SCIPsetIsLE(set, switchinglength, conflictstore->avgswitchlength) )
      return SCIP_OKAY;

   /* remove al conflicts depending on the cutoffbound */
   while( nseenconfs < conflictstore->nconflicts )
   {
      /* get the oldest conflict */
      assert(!SCIPqueueIsEmpty(conflictstore->orderqueue));
      idx = ((int) (size_t) SCIPqueueRemove(conflictstore->orderqueue)) - 1;
      assert(idx >= 0 && idx < conflictstore->conflictsize);

      if( conflictstore->conflicts[idx] == NULL )
      {
         /* add the id shifted by +1 to the queue of empty slots */
         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );
         continue;
      }

      /* get the oldest conflict */
      conflict = conflictstore->conflicts[idx];

      ++nseenconfs;

      /* we remove all as deleted marked constraints too */
      if( SCIPconsIsDeleted(conflict) )
      {
         /* release the constraint */
         SCIP_CALL( SCIPconsRelease(&conflict, blkmem, set) );

         conflictstore->ncbconflicts -= (SCIPsetIsInfinity(set, REALABS(conflictstore->primalbounds[idx])) ? 0 : 1);

         /* clean the conflict and primal bound array */
         conflictstore->conflicts[idx] = NULL;
         conflictstore->primalbounds[idx] = -SCIPsetInfinity(set);

         /* add the id shifted by +1 to the queue of empty slots */
         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );

         ++ndelconfs_del;
      }
      /* check if the conflict depends not on the cutoffbound */
      else if( SCIPsetIsInfinity(set, REALABS(conflictstore->primalbounds[idx])) )
      {
         SCIP_CALL( SCIPconsDelete(conflict, blkmem, set, stat, transprob) );
         SCIP_CALL( SCIPconsRelease(&conflict, blkmem, set) );

         conflictstore->conflicts[idx] = NULL;
         conflictstore->primalbounds[idx] = -SCIPsetInfinity(set);

         ++ndelconfs;

         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );
      }
      else
      {
         /* reinsert the id */
         SCIP_CALL( SCIPqueueInsert(conflictstore->orderqueue, (void*) (size_t) (idx+1)) );
      }
   }
   assert(conflictstore->ncbconflicts >= 0);

   SCIPdebugMessage("-> removed %d/%d conflicts, %d were already marked as deleted\n", ndelconfs+ndelconfs_del, conflictstore->nconflicts, ndelconfs_del);
   printf("-> removed %d/%d conflicts, %d were already marked as deleted\n", ndelconfs+ndelconfs_del, conflictstore->nconflicts, ndelconfs_del);
   conflictstore->nconflicts -= (ndelconfs+ndelconfs_del);

   return SCIP_OKAY;
}

/** delete all conflicts depending a cutoff bound larger than the given bound */
SCIP_RETCODE SCIPconflictstoreCleanBoundexceeding(
   SCIP_CONFLICTSTORE*   conflictstore,      /**< conflict storage */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic SCIP statistics */
   BMS_BLKMEM*           blkmem,             /**< block memory */
   SCIP_PROB*            transprob,          /**< transformed problem*/
   SCIP_Real             cutoffbound         /**< current cutoff bound */
   )
{
   SCIP_CONS* conflict;
   int nseenconfs;
   int ndelconfs;
   int ndelconfs_del;
   int idx;

   assert(conflictstore != NULL);
   assert(set != NULL);
   assert(stat != NULL);
   assert(blkmem != NULL);
   assert(transprob != NULL);

   nseenconfs = 0;
   ndelconfs = 0;
   ndelconfs_del = 0;

   /* return if we do not want to use the storage */
   if( set->conf_maxstoresize == -1 )
      return SCIP_OKAY;

   /* return if we do not want to remove conflicts related to an older cutoff bound */
   if( !set->conf_cleanboundexeedings )
      return SCIP_OKAY;

   /* remove al conflicts depending on the cutoffbound */
   while( nseenconfs < conflictstore->nconflicts )
   {
      /* get the oldest conflict */
      assert(!SCIPqueueIsEmpty(conflictstore->orderqueue));
      idx = ((int) (size_t) SCIPqueueRemove(conflictstore->orderqueue)) - 1;
      assert(idx >= 0 && idx < conflictstore->conflictsize);

      if( conflictstore->conflicts[idx] == NULL )
      {
         /* add the id shifted by +1 to the queue of empty slots */
         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );
         continue;
      }

      /* get the oldest conflict */
      conflict = conflictstore->conflicts[idx];

      ++nseenconfs;

      /* check if the conflict epends on the cutofbound */
      if( SCIPsetIsGT(set, conflictstore->primalbounds[idx], cutoffbound) )
      {
         SCIP_CALL( SCIPconsDelete(conflict, blkmem, set, stat, transprob) );
         SCIP_CALL( SCIPconsRelease(&conflict, blkmem, set) );

         conflictstore->conflicts[idx] = NULL;
         conflictstore->primalbounds[idx] = -SCIPsetInfinity(set);

         ++ndelconfs;
         --conflictstore->ncbconflicts;

         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );
      }
      /* we remove all as deleted marked constraints too */
      else if( SCIPconsIsDeleted(conflict) )
      {
         /* release the constraint */
         SCIP_CALL( SCIPconsRelease(&conflict, blkmem, set) );

         conflictstore->ncbconflicts -= (SCIPsetIsInfinity(set, REALABS(conflictstore->primalbounds[idx])) ? 0 : 1);

         /* clean the conflict and primal bound array */
         conflictstore->conflicts[idx] = NULL;
         conflictstore->primalbounds[idx] = -SCIPsetInfinity(set);

         /* add the id shifted by +1 to the queue of empty slots */
         SCIP_CALL( SCIPqueueInsert(conflictstore->slotqueue, (void*) (size_t) (idx+1)) );

         ++ndelconfs_del;
      }
      else
      {
         /* reinsert the id */
         SCIP_CALL( SCIPqueueInsert(conflictstore->orderqueue, (void*) (size_t) (idx+1)) );
      }
   }
   assert(conflictstore->ncbconflicts >= 0);

   SCIPdebugMessage("-> removed %d/%d conflicts, %d depending on cutoff bound\n", ndelconfs+ndelconfs_del, conflictstore->nconflicts, ndelconfs);
   conflictstore->nconflicts -= (ndelconfs+ndelconfs_del);

   return SCIP_OKAY;
}

SCIP_RETCODE SCIPconflictstoreDualRayStats(
   SCIP_CONFLICTSTORE*   conflictstore,      /**< conflict storage */
   int                   initsize,
   int                   heursize,
   int                   nconfsets,
   int                   nclauses
   )
{
   int maxsize;

   assert(conflictstore != NULL);

   maxsize = MAX(initsize, heursize);
   maxsize = MAX3(maxsize, nconfsets, nclauses);
   maxsize += 1;

   if( maxsize >= conflictstore->maxsize )
   {
      int i;

      if( conflictstore->maxsize == 0 )
      {
         SCIP_ALLOC( BMSallocMemoryArray(&conflictstore->dualrayinitsize, maxsize) );
         SCIP_ALLOC( BMSallocMemoryArray(&conflictstore->dualraysize, maxsize) );
         SCIP_ALLOC( BMSallocMemoryArray(&conflictstore->nconflictsets, maxsize) );
         SCIP_ALLOC( BMSallocMemoryArray(&conflictstore->nclauses, maxsize) );
      }
      else
      {
         SCIP_ALLOC( BMSreallocMemoryArray(&conflictstore->dualrayinitsize, maxsize) );
         SCIP_ALLOC( BMSreallocMemoryArray(&conflictstore->dualraysize, maxsize) );
         SCIP_ALLOC( BMSreallocMemoryArray(&conflictstore->nconflictsets, maxsize) );
         SCIP_ALLOC( BMSreallocMemoryArray(&conflictstore->nclauses, maxsize) );
      }

      /* init everything to 0 */
      for( i = conflictstore->maxsize; i < maxsize; i++ )
      {
         conflictstore->dualrayinitsize[i] = 0;
         conflictstore->dualraysize[i] = 0;
         conflictstore->nconflictsets[i] = 0;
         conflictstore->nclauses[i] = 0;

      }
      conflictstore->maxsize = maxsize;
   }

   if( initsize > 0 )
      ++conflictstore->dualrayinitsize[initsize];
   if( heursize > 0 )
      ++conflictstore->dualraysize[heursize];
   if( nconfsets > 0 )
      ++conflictstore->nconflictsets[nconfsets];
   if( nclauses > 0 )
      ++conflictstore->nclauses[nclauses];

   return SCIP_OKAY;
}
