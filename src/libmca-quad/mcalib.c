/********************************************************************************
 *                                                                              *
 *  This file is part of Verificarlo.                                           *
 *                                                                              *
 *  Copyright (c) 2015                                                          *
 *     Universite de Versailles St-Quentin-en-Yvelines                          *
 *     CMLA, Ecole Normale Superieure de Cachan                                 *
 *                                                                              *
 *  Verificarlo is free software: you can redistribute it and/or modify         *
 *  it under the terms of the GNU General Public License as published by        *
 *  the Free Software Foundation, either version 3 of the License, or           *
 *  (at your option) any later version.                                         *
 *                                                                              *
 *  Verificarlo is distributed in the hope that it will be useful,              *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of              *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the               *
 *  GNU General Public License for more details.                                *
 *                                                                              *
 *  You should have received a copy of the GNU General Public License           *
 *  along with Verificarlo.  If not, see <http://www.gnu.org/licenses/>.        *
 *                                                                              *
 ********************************************************************************/


// Changelog:
//
// 2015-05-20 replace random number generator with TinyMT64. This
// provides a reentrant, independent generator of better quality than
// the one provided in libc.
//
// 2015-05-20 New version based on quad flotting point type to replace MPFR until 
// required MCA precision is lower than quad mantissa divided by 2, i.e. 56 bits 
//

#include <math.h>
#include <mpfr.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>

#include "../common/quadmath-imp.h"
#include "libmca-quad.h"
#include "../vfcwrapper/vfcwrapper.h"
#include "../common/tinymt64.h"


#define NEAREST_FLOAT(x)	((float) (x))
#define	NEAREST_DOUBLE(x)	((double) (x))

int 	MCALIB_OP_TYPE 		= MCAMODE_IEEE;
int 	MCALIB_T		    = 53;

//possible qop values
#define QADD 1
#define QSUB 2
#define QMUL 3
#define QDIV 4

static float _mca_sbin(float a, float b, int qop);

static double _mca_dbin(double a, double b, int qop);

/******************** MCA CONTROL FUNCTIONS *******************
* The following functions are used to set virtual precision and
* MCA mode of operation.
***************************************************************/

static int _set_mca_mode(int mode){
	if (mode < 0 || mode > 3)
		return -1;

	MCALIB_OP_TYPE = mode;
	return 0;
}

static int _set_mca_precision(int precision){
	MCALIB_T = precision;
	return 0;
}


/******************** MCA RANDOM FUNCTIONS ********************
* The following functions are used to calculate the random
* perturbations used for MCA and apply these to MPFR format
* operands
***************************************************************/

/* random generator internal state */
tinymt64_t random_state;

static double _mca_rand(void) {
	/* Returns a random double in the (0,1) open interval */
	return tinymt64_generate_doubleOO(&random_state);
}

#define QINF_hx 0x7fff000000000000ULL 
#define QINF_lx 0x0000000000000000ULL

static __float128 pow2q(int exp) {
  __float128 res=0;
  uint64_t hx, lx;
  
  //specials
  if (exp == 0) return 1;
  if (exp > 16383) {
	SET_FLT128_WORDS64(res, QINF_hx, QINF_lx);
	return res;
  }
  if (exp <-16382) { /*subnormal*/
        SET_FLT128_WORDS64(res, ((uint64_t) 0 ) , ((uint64_t) 1 ) << exp);
        return res;
  }
  
  //normal case
  hx=( ((uint64_t) exp) + 16382) << 48;
  lx=0;
  SET_FLT128_WORDS64(res, hx, QINF_lx);
  return res;
}

static uint32_t rexpq (__float128 x)
{
  //no need to check special value in our cases since pow2q will deal with it
  //do not reuse it outside this code!
  uint64_t hx,ix;
  uint32_t exp=0;
  GET_FLT128_MSW64(hx,x);
  ix = hx&0x7fffffffffffffffULL;
  exp += (ix>>48)-16382;
  return exp;
}

static int _mca_inexact(__float128 *qa) {
	

	if (MCALIB_OP_TYPE == MCAMODE_IEEE) {
		return 0;
	}
	
	//shall we remove it to remove the if for all other values?
	//1% improvment on kahan => is better or worth on other benchmarks?
	//if (qa == 0) {
	//	return 0;
	//}
	
	int32_t e_a=0;
	//frexpq (*a, &e_a);
	e_a=rexpq(*qa);
	int32_t e_n = e_a - (MCALIB_T - 1);
	double d_rand = (_mca_rand() - 0.5);
	//can we use bits manipulation instead of qmul?
	//idea: use one of the bit of d_rand for sign such that drand is between -1 and 1, and remove 1 to e_n to compensate
	//This bit should be uniformly distributed
	//build the quad to add using e_n, the mantissa of d_rand and the new sign bit => get ride of the mul...
	*qa = *qa + pow2q(e_n)*d_rand;
}

static void _mca_seed(void) {
	const int key_length = 3;
	uint64_t init_key[key_length];
	struct timeval t1;
	gettimeofday(&t1, NULL);

	/* Hopefully the following seed is good enough for Montercarlo */
	init_key[0] = t1.tv_sec;
	init_key[1] = t1.tv_usec;
	init_key[2] = getpid();

	tinymt64_init_by_array(&random_state, init_key, key_length);
}

/******************** MCA ARITHMETIC FUNCTIONS ********************
* The following set of functions perform the MCA operation. Operands
* are first converted to quad  format (GCC), inbound and outbound 
* perturbations are applied using the _mca_inexact function, and the 
* result converted to the original format for return
*******************************************************************/

static float _mca_sbin(float a, float b,int  qop) {
	
	__float128 qa=(__float128)a;
	__float128 qb=(__float128)b;	


	__float128 res=0;

	if (MCALIB_OP_TYPE != MCAMODE_RR) {
		_mca_inexact(&qa);
		_mca_inexact(&qb);
	}

	switch (qop){

		case QADD:
  			res=qa+qb;
  		break;

		case QMUL:
  			res=qa*qb;
  		break;

		case QSUB:
  			res=qa-qb;
  		break;

		case QDIV:
  			res=qa/qb;
  		break;

		default:
  		perror("invalid operator in mca_quad!!!\n");
  		abort();
	}

	if (MCALIB_OP_TYPE != MCAMODE_PB) {
		_mca_inexact(&res);
	}

	return NEAREST_FLOAT(res);
}


static double _mca_dbin(double a, double b, int qop) {
	__float128 qa=(__float128)a;
	__float128 qb=(__float128)b;	
	__float128 res=0;

	if (MCALIB_OP_TYPE != MCAMODE_RR) {
		_mca_inexact(&qa);
		_mca_inexact(&qb);
	}

	switch (qop){

		case QADD:
  			res=qa+qb;
  		break;

		case QMUL:
  			res=qa*qb;
  		break;

		case QSUB:
  			res=qa-qb;
  		break;

		case QDIV:
  			res=qa/qb;
  		break;

		default:
  		perror("invalid operator in mca_quad!!!\n");
  		abort();
	}

	if (MCALIB_OP_TYPE != MCAMODE_PB) {
		_mca_inexact(&res);
	}

	return NEAREST_DOUBLE(res);

}


/******************** MCA COMPARE FUNCTIONS ********************
* Compare operations do not require MCA 
****************************************************************/


/************************* FPHOOKS FUNCTIONS *************************
* These functions correspond to those inserted into the source code
* during source to source compilation and are replacement to floating
* point operators
**********************************************************************/

static float _floatadd(float a, float b) {
	//return a + b
	return _mca_sbin(a, b,QADD);
}

static float _floatsub(float a, float b) {
	//return a - b
	return _mca_sbin(a, b, QSUB);
}

static float _floatmul(float a, float b) {
	//return a * b
	return _mca_sbin(a, b, QMUL);
}

static float _floatdiv(float a, float b) {
	//return a / b
	return _mca_sbin(a, b, QDIV);
}


static double _doubleadd(double a, double b) {
	//return a + b
	return _mca_dbin(a, b, QADD);
}

static double _doublesub(double a, double b) {
	//return a - b
	return _mca_dbin(a, b, QSUB);
}

static double _doublemul(double a, double b) {
	//return a * b
	return _mca_dbin(a, b, QMUL);
}

static double _doublediv(double a, double b) {
	//return a / b
	return _mca_dbin(a, b, QDIV);
}


struct mca_interface_t quad_mca_interface = {
	_floatadd,
	_floatsub,
	_floatmul,
	_floatdiv,
	_doubleadd,
	_doublesub,
	_doublemul,
	_doublediv,
	_mca_seed,
	_set_mca_mode,
	_set_mca_precision
};