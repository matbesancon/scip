/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2004 Tobias Achterberg                              */
/*                                                                           */
/*                  2002-2004 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the SCIP Academic Licence.        */
/*                                                                           */
/*  You should have received a copy of the SCIP Academic License             */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma ident "@(#) $Id: branch_allfullstrong.c,v 1.3 2004/04/27 15:49:56 bzfpfend Exp $"

/**@file   branch_allfullstrong.c
 * @brief  all variables full strong LP branching rule
 * @author Tobias Achterberg
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include <assert.h>
#include <string.h>

#include "branch_allfullstrong.h"


#define BRANCHRULE_NAME          "allfullstrong"
#define BRANCHRULE_DESC          "all variables full strong branching"
#define BRANCHRULE_PRIORITY      -1000
#define BRANCHRULE_MAXDEPTH      -1


/** branching rule data */
struct BranchruleData
{
   int              lastcand;           /**< last evaluated candidate of last branching rule execution */
};



/** performs the all fullstrong branching */
static
RETCODE branch(
   SCIP*            scip,               /**< SCIP data structure */
   BRANCHRULE*      branchrule,         /**< branching rule */
   RESULT*          result              /**< pointer to store the result of the callback method */
   )
{
   BRANCHRULEDATA* branchruledata;
   VAR** pseudocands;
   Real cutoffbound;
   Real lowerbound;
   Real bestdown;
   Real bestup;
   Real bestscore;
   Bool allcolsinlp;
   int npseudocands;
   int npriopseudocands;
   int bestpseudocand;

   assert(branchrule != NULL);
   assert(strcmp(SCIPbranchruleGetName(branchrule), BRANCHRULE_NAME) == 0);
   assert(scip != NULL);
   assert(result != NULL);

   /* get branching rule data */
   branchruledata = SCIPbranchruleGetData(branchrule);
   assert(branchruledata != NULL);

   /* get current lower objective bound of the local sub problem and global cutoff bound */
   lowerbound = SCIPgetLocalLowerbound(scip);
   cutoffbound = SCIPgetCutoffbound(scip);

   /* check, if all existing columns are in LP, and thus the strong branching results give lower bounds */
   allcolsinlp = SCIPallColsInLP(scip);

   /* get all non-fixed variables (not only the fractional ones) */
   CHECK_OKAY( SCIPgetPseudoBranchCands(scip, &pseudocands, &npseudocands, &npriopseudocands) );
   assert(npseudocands > 0);
   assert(npriopseudocands > 0);

   /* if only one candidate exists, choose this one without applying strong branching */
   bestpseudocand = 0;
   bestdown = lowerbound;
   bestup = lowerbound;
   bestscore = -SCIPinfinity(scip);
   if( npseudocands > 1 )
   {
      Real solval;
      Real down;
      Real up;
      Real downgain;
      Real upgain;
      Real score;
      Bool integral;
      Bool lperror;
      Bool downinf;
      Bool upinf;
      int i;
      int c;

      /* search the full strong candidate */
      for( i = 0; i < npseudocands; ++i )
      {
         /* cycle through the candidates, starting with the position evaluated in the last run */
         c = (i+branchruledata->lastcand) % npseudocands;
         assert(pseudocands[c] != NULL);

         /* we can only apply strong branching on COLUMN variables */
         if( SCIPvarGetStatus(pseudocands[c]) != SCIP_VARSTATUS_COLUMN )
            continue;

         solval = SCIPvarGetLPSol(pseudocands[c]);
         integral = SCIPisIntegral(scip, solval);

         debugMessage("applying strong branching on %s variable <%s>[%g,%g] with solution %g\n",
            integral ? "integral" : "fractional", SCIPvarGetName(pseudocands[c]), SCIPvarGetLbLocal(pseudocands[c]), 
            SCIPvarGetUbLocal(pseudocands[c]), solval);

         CHECK_OKAY( SCIPgetVarStrongbranch(scip, pseudocands[c], INT_MAX, &down, &up, &lperror) );

         /* check for an error in strong branching */
         if( lperror )
         {
            SCIPmessage(scip, SCIP_VERBLEVEL_HIGH,
               "(node %lld) error in strong branching call for variable <%s> with solution %g\n", 
               SCIPgetNNodes(scip), SCIPvarGetName(pseudocands[c]), solval);
            break;
         }

         /* evaluate strong branching */
         down = MAX(down, lowerbound);
         up = MAX(up, lowerbound);
         downinf = SCIPisGE(scip, down, cutoffbound);
         upinf = SCIPisGE(scip, up, cutoffbound);
         downgain = down - lowerbound;
         upgain = up - lowerbound;

         if( allcolsinlp )
         {
            /* because all existing columns are in LP, the strong branching bounds are feasible lower bounds */
            if( downinf && upinf )
            {
               if( integral )
               {
                  Bool infeasible;
                  Bool fixed;

                  /* both bound changes are infeasible: variable can be fixed to its current value */
                  CHECK_OKAY( SCIPfixVar(scip, pseudocands[c], solval, &infeasible, &fixed) );
                  assert(!infeasible);
                  assert(fixed);
                  *result = SCIP_REDUCEDDOM;
                  debugMessage(" -> integral variable <%s> is infeasible in both directions\n",
                     SCIPvarGetName(pseudocands[c]));
                  break; /* terminate initialization loop, because LP was changed */
               }
               else
               {
                  /* both roundings are infeasible: the node is infeasible */
                  *result = SCIP_CUTOFF;
                  debugMessage(" -> fractional variable <%s> is infeasible in both directions\n",
                     SCIPvarGetName(pseudocands[c]));
                  break; /* terminate initialization loop, because node is infeasible */
               }
            }
            else if( downinf )
            {
               /* downwards rounding is infeasible -> change lower bound of variable to upward rounding */
               Real newlb = SCIPceil(scip, solval);

               if( SCIPvarGetLbLocal(pseudocands[c]) < newlb - 0.5 )
               {
                  CHECK_OKAY( SCIPchgVarLb(scip, pseudocands[c], newlb) );
                  *result = SCIP_REDUCEDDOM;
                  debugMessage(" -> variable <%s> is infeasible in downward branch\n", SCIPvarGetName(pseudocands[c]));
                  break; /* terminate initialization loop, because LP was changed */
               }
            }
            else if( upinf )
            {
               /* upwards rounding is infeasible -> change upper bound of variable to downward rounding */
               Real newub = SCIPfloor(scip, solval);

               if( SCIPvarGetUbLocal(pseudocands[c]) > newub + 0.5 )
               {
                  CHECK_OKAY( SCIPchgVarUb(scip, pseudocands[c], newub) );
                  *result = SCIP_REDUCEDDOM;
                  debugMessage(" -> variable <%s> is infeasible in upward branch\n", SCIPvarGetName(pseudocands[c]));
                  break; /* terminate initialization loop, because LP was changed */
               }
            }
         }

         /* check for a better score, if we are within the maximum priority candidates */
         if( c < npriopseudocands )
         {
            if( integral )
            {
               Real gains[3];

               gains[0] = downgain;
               gains[1] = 0.0;
               gains[2] = upgain;
               score = SCIPgetBranchScoreMultiple(scip, pseudocands[c], 3, gains);
            }
            else
               score = SCIPgetBranchScore(scip, pseudocands[c], downgain, upgain);

            if( score > bestscore )
            {
               bestpseudocand = c;
               bestdown = down;
               bestup = up;
               bestscore = score;
            }
         }

         /* update pseudo cost values */
         if( !downinf )
         {
            CHECK_OKAY( SCIPupdateVarPseudocost(scip, pseudocands[c], solval-SCIPceil(scip, solval-1.0), downgain, 1.0) );
         }
         if( !upinf )
         {
            CHECK_OKAY( SCIPupdateVarPseudocost(scip, pseudocands[c], solval-SCIPfloor(scip, solval+1.0), upgain, 1.0) );
         }

         debugMessage(" -> var <%s> (solval=%g, downgain=%g, upgain=%g, score=%g) -- best: <%s> (%g)\n",
            SCIPvarGetName(pseudocands[c]), solval, downgain, upgain, score,
            SCIPvarGetName(pseudocands[bestpseudocand]), bestscore);
      }

      /* remember last evaluated candidate */
      branchruledata->lastcand = c;
   }

   if( *result != SCIP_CUTOFF && *result != SCIP_REDUCEDDOM )
   {
      NODE* node;
      VAR* var;
      Real solval;
      Real lb;
      Real ub;
      Real newlb;
      Real newub;

      assert(*result == SCIP_DIDNOTRUN);
      assert(0 <= bestpseudocand && bestpseudocand < npseudocands);

      var = pseudocands[bestpseudocand];
      solval = SCIPvarGetLPSol(var);
      lb = SCIPvarGetLbLocal(var);
      ub = SCIPvarGetUbLocal(var);
      
      /* perform the branching */
      debugMessage(" -> %d candidates, selected candidate %d: variable <%s>[%g,%g] (solval=%g, down=%g, up=%g, score=%g)\n",
         npseudocands, bestpseudocand, SCIPvarGetName(var), lb, ub, solval, bestdown, bestup, bestscore);

      /* create child node with x <= ceil(x'-1) */
      newub = SCIPceil(scip, solval-1.0);
      if( newub >= lb - 0.5 )
      {
         debugMessage(" -> creating child: <%s> <= %g\n", SCIPvarGetName(var), newub);
         CHECK_OKAY( SCIPcreateChild(scip, &node) );
         CHECK_OKAY( SCIPchgVarUbNode(scip, node, var, newub) );
         if( allcolsinlp )
         {
            CHECK_OKAY( SCIPupdateNodeLowerbound(scip, node, bestdown) );
         }
         debugMessage(" -> child's lowerbound: %g\n", SCIPnodeGetLowerbound(node));
      }

      /* if the solution was integral, create child x == x' */
      if( SCIPisIntegral(scip, solval) )
      {
         assert(solval > lb + 0.5 || solval < ub - 0.5); /* otherwise, the variable is already fixed */

         debugMessage(" -> creating child: <%s> == %g\n", SCIPvarGetName(var), solval);
         CHECK_OKAY( SCIPcreateChild(scip, &node) );
         if( solval > lb + 0.5 )
         {
            CHECK_OKAY( SCIPchgVarLbNode(scip, node, var, solval) );
         }
         if( solval < ub - 0.5 )
         {
            CHECK_OKAY( SCIPchgVarUbNode(scip, node, var, solval) );
         }
         debugMessage(" -> child's lowerbound: %g\n", SCIPnodeGetLowerbound(node));
      }

      /* create child node with x >= floor(x'+1) */
      newlb = SCIPfloor(scip, solval+1.0);
      if( newlb <= ub + 0.5 )
      {
         debugMessage(" -> creating child: <%s> >= %g\n", SCIPvarGetName(var), newlb);
         CHECK_OKAY( SCIPcreateChild(scip, &node) );
         CHECK_OKAY( SCIPchgVarLbNode(scip, node, var, newlb) );
         if( allcolsinlp )
         {
            CHECK_OKAY( SCIPupdateNodeLowerbound(scip, node, bestup) );
         }
         debugMessage(" -> child's lowerbound: %g\n", SCIPnodeGetLowerbound(node));
      }

      *result = SCIP_BRANCHED;
   }

   return SCIP_OKAY;
}




/*
 * Callback methods
 */

/** destructor of branching rule to free user data (called when SCIP is exiting) */
static
DECL_BRANCHFREE(branchFreeAllfullstrong)
{  /*lint --e{715}*/
   BRANCHRULEDATA* branchruledata;

   /* free branching rule data */
   branchruledata = SCIPbranchruleGetData(branchrule);
   SCIPfreeMemory(scip, &branchruledata);
   SCIPbranchruleSetData(branchrule, NULL);

   return SCIP_OKAY;
}


/** initialization method of branching rule (called after problem was transformed) */
static
DECL_BRANCHINIT(branchInitAllfullstrong)
{
   BRANCHRULEDATA* branchruledata;

   /* init branching rule data */
   branchruledata = SCIPbranchruleGetData(branchrule);
   branchruledata->lastcand = 0;

   return SCIP_OKAY;
}


/** deinitialization method of branching rule (called before transformed problem is freed) */
#define branchExitAllfullstrong NULL


/** branching execution method for fractional LP solutions */
static
DECL_BRANCHEXECLP(branchExeclpAllfullstrong)
{
   assert(result != NULL);

   debugMessage("Execlp method of allfullstrong branching\n");

   *result = SCIP_DIDNOTRUN;
   
   CHECK_OKAY( branch(scip, branchrule, result) );

   return SCIP_OKAY;
}


/** branching execution method for not completely fixed pseudo solutions */
static
DECL_BRANCHEXECPS(branchExecpsAllfullstrong)
{
   assert(result != NULL);

   debugMessage("Execps method of allfullstrong branching\n");

   *result = SCIP_DIDNOTRUN;

   if( SCIPhasActNodeLP(scip) )
   {
      CHECK_OKAY( branch(scip, branchrule, result) );
   }

   return SCIP_OKAY;
}




/*
 * branching specific interface methods
 */

/** creates the all variables full strong LP braching rule and includes it in SCIP */
RETCODE SCIPincludeBranchruleAllfullstrong(
   SCIP*            scip                /**< SCIP data structure */
   )
{
   BRANCHRULEDATA* branchruledata;

   /* create allfullstrong branching rule data */
   CHECK_OKAY( SCIPallocMemory(scip, &branchruledata) );
   branchruledata->lastcand = 0;

   /* include allfullstrong branching rule */
   CHECK_OKAY( SCIPincludeBranchrule(scip, BRANCHRULE_NAME, BRANCHRULE_DESC, BRANCHRULE_PRIORITY, BRANCHRULE_MAXDEPTH,
                  branchFreeAllfullstrong, branchInitAllfullstrong, branchExitAllfullstrong, 
                  branchExeclpAllfullstrong, branchExecpsAllfullstrong,
                  branchruledata) );

   return SCIP_OKAY;
}
