/*
 * @author Marc Suchard
 */

#include "GPU/GPUImplDefs.h"
#include "device_functions.h"

#include "kernelSpmvCoo.cu"
#include "kernelSpmvCsr.cu"

#include <cmath>
#include "cuda.h"

#define multBy4(x)	(x << 2)
#define multBy16(x)	(x << 4)

#if __APPLE__
namespace util { 
//
// We have to emulate FP atomicAdd because it isn't supported natively
// on devices prior to capability 2.0.
//
// Note that the semantics of atomicCAS() on float values is a little
// dodgy.  It just does a bit comparison rather than a true floating
// point comparison.  Hence 0 != -0, for instance.
//
static __inline__ __device__ float atomicAdd(float *addr, float val)
{
    float old=*addr, assumed;
    
    do {
        assumed = old;
        old = int_as_float( atomicCAS((int*)addr,
                                        float_as_int(assumed),
                                        float_as_int(val+assumed)));
    } while( float_as_int(assumed)!=float_as_int(old) );
    //
    // NOTE: Comparing as ints rather than floats is mandatory.
    // For floats, the test NaN==NaN would always fall, leading to an
    // infinite loop.

    return old;
}

} // end namespace device

#endif // __APPLE__

#ifdef __cplusplus
extern "C" {
#endif

#include "kernelReductions.cu"

	__global__ void kernelClear(REAL* dest, int length) {
		int idx = blockIdx.x * CLEAR_MEMORY_BLOCK_SIZE + threadIdx.x;
		if (idx < length) {
			dest[idx] = 0.0;
		}
	}
	
	__global__ void kernelUpdateXBeta(REAL *xBeta,
									  int *xIColumn,
								      int length,
								      REAL delta) {								      
								      
		int idx = blockIdx.x * UPDATE_XBETA_BLOCK_SIZE + threadIdx.x;
		if (idx < length) {
			int k = xIColumn[idx];
			xBeta[k] += delta; // All 'index' values are unique
		}		
	}
	
    __global__ void kernelComputeNumerator(
                                    const REAL *offsExpXBeta,
                                    REAL *numer,
                                    const int *rows,
                                    const int *cols,                                    
                                    int lengthX,
                                    int lengthN) {
                                    
        int idx = blockIdx.x * UPDATE_NUMERATOR_BLOCK_SIZE + threadIdx.x;
        
#ifdef MERGE_CLEAR
        // Clear numer
        int i = idx;
        while (i < lengthN) {
            numer[i] = 0.0;
            i += gridDim.x * UPDATE_NUMERATOR_BLOCK_SIZE;
        }
#endif
        
        if (idx < lengthX) {
            int k = cols[idx];
            int n = rows[idx];
                
#ifndef DOUBLE_PRECISION
#if __APPLE__
		    util::atomicAdd(&numer[n], offsExpXBeta[k]);
#else
			atomicAdd(&numer[n], offsExpXBeta[k]);	
#endif
#else
			atomicAdd(&numer[n], offsExpXBeta[k]);
#endif                                                                        
		}
    }

	__global__ void kernelUpdateXBetaAndFriends(
									  REAL *xBeta,
									  REAL *offsExpXBeta,
									  REAL *denomPid,
									  int *rowOffs,
									  int *otherOffs,
									  int *offs,
									  int *xIColumn,
								      int length,
								      REAL delta) {								      
								      
		int idx = blockIdx.x * UPDATE_XBETA_AND_FRIENDS_BLOCK_SIZE + threadIdx.x;
		if (idx < length) {
			int k = xIColumn[idx];
			int n = rowOffs[idx];
			
			REAL xb = xBeta[k] + delta; // Compute new xBeta			
			REAL newOffsExpXBeta = offs[k] * exp(xb);
			REAL oldOffsExpXBeta = offsExpXBeta[k];	
			offsExpXBeta[k] = newOffsExpXBeta;		 
			
			// Store new values
			xBeta[k] = xb;				
			
		#ifndef DOUBLE_PRECISION
		#if __APPLE__
		    	util::atomicAdd(&denomPid[n], (newOffsExpXBeta - oldOffsExpXBeta));
		#else
			atomicAdd(&denomPid[n], (newOffsExpXBeta - oldOffsExpXBeta));	
		#endif
		#else
			atomicAdd(&denomPid[n], (newOffsExpXBeta - oldOffsExpXBeta));
		#endif					
		}					
	}	
	
	__global__ void kernelComputeIntermediates(REAL *offsExpXBeta,
											   REAL *denomPid, // TODO Remove
									           int *offs, // TODO Remove
									           REAL *xBeta,
									           int *pid, // TODO Remove									           
									           int length) {
									                   
        int idx = blockIdx.x * COMPUTE_INTERMEDIATES_BLOCK_SIZE + threadIdx.x;         
        if (idx < length) {      
	    	offsExpXBeta[idx] = offs[idx] * exp(xBeta[idx]);
 	    }								           
    }
	
	__global__ void kernelComputeIntermediatesMoreWork(REAL *offsExpXBeta,
											   REAL *denomPid, // TODO Remove
									           int *offs, // TODO Remove
									           REAL *xBeta,
									           int *pid, // TODO Remove									           
									           int length) {
									                   
        int idx = blockIdx.x * COMPUTE_INTERMEDIATES_BLOCK_SIZE + threadIdx.x;
        while (idx < length) {
	    	offsExpXBeta[idx] = offs[idx] * exp(xBeta[idx]);							          
	    	idx += gridDim.x * COMPUTE_INTERMEDIATES_BLOCK_SIZE;
		}
	}	
	
	__global__ void kernelComputeRatio(
			REAL *numerPid,
			REAL *denomPid,
			REAL *t1,
			int length) {
		int idx = blockIdx.x * MAKE_RATIO_BLOCK_SIZE + threadIdx.x;
		if (idx < length) {
			t1[idx] = numerPid[idx] / denomPid[idx];
		}
	}
	
	__global__ void kernelComputeGradientNoFuse(
			REAL *t1,
			int *nEvents,
			REAL *gradient,			
			int length) {
		int idx = blockIdx.x * MAKE_RATIO_BLOCK_SIZE + threadIdx.x;
		if (idx < length) {
			gradient[idx] = nEvents[idx] * t1[idx];			
		}
	}	
	
	__global__ void kernelComputeHessianNoFuse(
			REAL *t1,
			int *nEvents,			
			REAL *hessian,
			int length) {
		int idx = blockIdx.x * MAKE_RATIO_BLOCK_SIZE + threadIdx.x;
		if (idx < length) {
			hessian[idx] = nEvents[idx] * t1[idx] * (1.0 - t1[idx]);
		}
	}	

	__global__ void kernelComputeGradientHessian(
			REAL *numerPid,
			REAL *denomPid,
			int *nEvents,
			REAL *gradient,
			REAL *hessian,
			int length) {
		int idx = blockIdx.x * MAKE_RATIO_BLOCK_SIZE + threadIdx.x;
		if (idx < length) {
			REAL ratio = numerPid[idx] / denomPid[idx];
			int nEvent = nEvents[idx];
			REAL g = nEvent * ratio;
			gradient[idx] = g;
			hessian[idx] = g * (1.0 - ratio);
		}
	}

	__global__ void kernelReduceAll(REAL *offsExpXBeta,
									REAL *denomPid,									
									int *rowOffsets,									           
									int length) {
									
		int idx = blockIdx.x * BLOCK_SIZE_REDUCE_ALL + threadIdx.x;
		if (idx < length) {
			REAL sum = 0;
			int start = rowOffsets[idx];
			int stop = rowOffsets[idx + 1];
			for (int i = start; i < stop; i++) {
				sum += offsExpXBeta[i];
			}
			denomPid[idx] = sum;			
		}																           						           
	}

#define SDATA_XWU(x,z,y)	sdata[y]
#define BLOCKSIZE	512

__global__ void kernelReduceTwo(
		REAL* out,
		REAL* x,
		unsigned int totalN) {

	const unsigned int tid = threadIdx.x;
	const unsigned int dim = blockIdx.x;
	unsigned int i = tid;

	REAL mySum = 0;

	while (i < totalN) {
		mySum += x[dim * totalN + i]; // TODO Could reduce two at a time
		i += BLOCKSIZE;
	}

//	REAL *sdata = SharedMemory<REAL>();
	__shared__ REAL sdata[BLOCKSIZE];
	SDATA_XWU(pred, ch, tid) = mySum;
	__syncthreads();

#if (BLOCKSIZE >= 512)
	if (tid < 256) { SDATA_XWU(pred, ch, tid) = mySum = mySum + SDATA_XWU(pred, ch, tid + 256); } __syncthreads();
#endif
#if (BLOCKSIZE >= 256)
	if (tid < 128) { SDATA_XWU(pred, ch, tid) = mySum = mySum + SDATA_XWU(pred, ch, tid + 128); } __syncthreads();
#endif
#if (BLOCKSIZE >= 128)
	if (tid <  64) { SDATA_XWU(pred, ch, tid) = mySum = mySum + SDATA_XWU(pred, ch, tid +  64); } __syncthreads();
#endif

    if (tid < 32) {
#if (BLOCKSIZE >=  64)
    	SDATA_XWU(pred, ch, tid) = mySum = mySum + SDATA_XWU(pred, ch, tid + 32); __syncthreads();
#endif
#if (BLOCKSIZE >=  32)
    	SDATA_XWU(pred, ch, tid) = mySum = mySum + SDATA_XWU(pred, ch, tid + 16); __syncthreads();
#endif
#if (BLOCKSIZE >=  16)
    	SDATA_XWU(pred, ch, tid) = mySum = mySum + SDATA_XWU(pred, ch, tid +  8); __syncthreads();
#endif
#if (BLOCKSIZE >=   8)
    	SDATA_XWU(pred, ch, tid) = mySum = mySum + SDATA_XWU(pred, ch, tid +  4); __syncthreads();
#endif
#if (BLOCKSIZE >=   4)
    	SDATA_XWU(pred, ch, tid) = mySum = mySum + SDATA_XWU(pred, ch, tid +  2); __syncthreads();
#endif
#if (BLOCKSIZE >=   2)
    	SDATA_XWU(pred, ch, tid) = mySum = mySum + SDATA_XWU(pred, ch, tid +  1); __syncthreads();
#endif
    }

	if (tid == 0) {
		out[dim] = SDATA_XWU(pred, ch, 0);
	}
}

#ifdef __cplusplus	
} // extern "C"
#endif

