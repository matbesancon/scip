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
#pragma ident "@(#) $Id: type_scip.h,v 1.3 2004/04/27 15:50:06 bzfpfend Exp $"

/**@file   type_scip.h
 * @brief  type definitions for SCIP's main datastructure
 * @author Tobias Achterberg
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __TYPE_SCIP_H__
#define __TYPE_SCIP_H__


/** SCIP operation stage */
enum Stage
{
   SCIP_STAGE_INIT         =  0,        /**< SCIP datastructures are initialized, no problem exists */
   SCIP_STAGE_PROBLEM      =  1,        /**< the problem is being created and modified */
   SCIP_STAGE_TRANSFORMING =  2,        /**< the problem is being transformed into solving data space */
   SCIP_STAGE_TRANSFORMED  =  3,        /**< the problem was transformed into solving data space */
   SCIP_STAGE_PRESOLVING   =  4,        /**< the problem is being presolved */
   SCIP_STAGE_PRESOLVED    =  5,        /**< the problem was presolved */
   SCIP_STAGE_INITSOLVE    =  6,        /**< the solving process data is being initialized */
   SCIP_STAGE_SOLVING      =  7,        /**< the problem is being solved */
   SCIP_STAGE_SOLVED       =  8,        /**< the problem was solved */
   SCIP_STAGE_FREESOLVE    =  9,        /**< the solving process data is being freed */
   SCIP_STAGE_FREETRANS    = 10         /**< the transformed problem is being freed */
};
typedef enum Stage STAGE;

typedef struct Scip SCIP;               /**< SCIP main data structure */


#endif
