#include <stdio.h>
#include <iostream>
#include <chrono>

#include <cub/cub.cuh>
#include <thrust/device_vector.h>
#include <thrust/device_ptr.h>

#include "CudaKernel.h"

using namespace cub;
	
template <typename RealType>
__global__ void kernelUpdateXBeta(int offX, int offK, const int taskCount, RealType delta,
                const RealType* d_X, const int* K, RealType* d_XBeta, RealType* d_ExpXBeta)
//__global__ void kernelUpdateXBeta(RealType* d_X, RealType* d_XBeta, RealType* d_ExpXBeta, RealType delta, int N)
{
    int task = blockIdx.x * blockDim.x + threadIdx.x;

    //if (formatType == INDICATOR || formatType == SPARSE) {
	int k = K[offK + task];
    //} else { // DENSE, INTERCEPT
//	int k = task;
    //}

    //if (formatType == SPARSE || formatType == DENSE) {
//	RealType inc = delta * d_X[offX + task];
    //} else { // INDICATOR, INTERCEPT
	RealType inc = delta;
    //}

    if (task < taskCount) {
	RealType xb = d_XBeta[k] + inc;
        d_XBeta[k] = xb;
	d_ExpXBeta[k] = expf(xb);
    }
}

template <typename RealType>
__global__ void kernelComputeGradientAndHessian(RealType* d_BufferG, RealType* d_BufferH, RealType* d_AccNumer, RealType* d_AccNumer2, RealType* d_AccDenom, RealType* d_NWeight, int N)
{
    int task = blockIdx.x * blockDim.x + threadIdx.x;

    if (task < N) {
        RealType t = d_AccNumer[task] / d_AccDenom[task];
        RealType g = d_NWeight[task] * t;
        d_BufferG[task] = g;
        //if (IteratorType::isIndicator) {
            d_BufferH[task] = g * (1.0 - t);
        //} else {
//	    d_BufferH[task] = d_NWeight[task] * (d_AccNumer2[task] / d_AccDenom[task] - t * t);
        //}
    }
}


template <class RealType>
CudaKernel<RealType>::CudaKernel()
{ 
	std::cout << "CUDA class Created \n";
}

template <class RealType>
CudaKernel<RealType>::~CudaKernel()
{
/*
    cudaFree(d_XBeta);
    cudaFree(d_ExpXBeta);
    cudaFree(d_AccDenom);

    cudaFree(d_Numer);
    cudaFree(d_Numer2);
    cudaFree(d_AccDenom);
    cudaFree(d_AccNumer);
    cudaFree(d_AccNumer2);
    cudaFree(d_NWeight);
    cudaFree(d_BufferG);
    cudaFree(d_BufferH);
    cudaFree(d_Gradient);
    cudaFree(d_Hessian);
*/
    std::cout << "CUDA class Destroyed \n";
}

template <class RealType>
void CudaKernel<RealType>::initialize(int K, int N)
{

	//TODO use thrust	
    cudaMalloc(&d_XBeta,  sizeof(RealType) * K);
    cudaMalloc(&d_ExpXBeta,  sizeof(RealType) * K);

    cudaMalloc(&d_Numer,  sizeof(RealType) * N);
    cudaMalloc(&d_Numer2,  sizeof(RealType) * N);

    cudaMalloc(&d_AccDenom, sizeof(RealType) * N);
    cudaMalloc(&d_AccNumer, sizeof(RealType) * N);
    cudaMalloc(&d_AccNumer2, sizeof(RealType) * N);

    cudaMalloc(&d_BufferG, sizeof(RealType) * N);
    cudaMalloc(&d_BufferH, sizeof(RealType) * N);
    cudaMalloc(&d_Gradient, sizeof(RealType));
    cudaMalloc(&d_Hessian, sizeof(RealType));

    cudaMalloc(&d_NWeight, sizeof(RealType) * N);    
    
    std::cout << "Initialize CUDA vector \n";
}

template <class RealType>
void CudaKernel<RealType>::updateXBeta(const thrust::device_vector<RealType>& X, const thrust::device_vector<int>& K, unsigned int offX, unsigned int offK, const unsigned int taskCount, RealType delta, thrust::device_vector<RealType>& dXBeta, thrust::device_vector<RealType>& dExpXBeta, int gridSize, int blockSize)
{
//    auto start1 = std::chrono::steady_clock::now();
	/*
            for(int i = 0; i < dXBeta.size(); i++) {
                    std::cout << "old i: " << i << " xb: "  << dXBeta[i] << " exb: " << dExpXBeta[i] << std::endl;
            }
*/
    kernelUpdateXBeta<<<gridSize, blockSize>>>(offX, offK, taskCount, delta, thrust::raw_pointer_cast(&X[0]), thrust::raw_pointer_cast(&K[0]), thrust::raw_pointer_cast(&dXBeta[0]), thrust::raw_pointer_cast(&dExpXBeta[0]));
    /*
            for(int i = 0; i < dXBeta.size(); i++) {
                    std::cout << "new i: " << i << " xb: "  << dXBeta[i] << " exb: " << dExpXBeta[i] << std::endl;
            }
	    */
//    auto end1 = std::chrono::steady_clock::now();
//    timerG1 += std::chrono::duration<double, std::milli>(end1 - start1).count();
}

template <class RealType>
void CudaKernel<RealType>::computeGradientAndHessian(size_t& N, int& gridSize, int& blockSize)
{
//    auto start1 = std::chrono::steady_clock::now();

    kernelComputeGradientAndHessian<<<gridSize, blockSize>>>(d_BufferG, d_BufferH, d_AccNumer, d_AccNumer2, d_AccDenom, d_NWeight, N);

    CudaKernel<RealType>::CubReduce(d_BufferG, d_Gradient, N);
    CudaKernel<RealType>::CubReduce(d_BufferH, d_Hessian, N);

//    auto end1 = std::chrono::steady_clock::now();
//    timerG1 += std::chrono::duration<double, std::milli>(end1 - start1).count();
}



template <class RealType>
void CudaKernel<RealType>::CubReduce(RealType* d_in, RealType* d_out, int num_items)
{
    // Allocate temporary storage
    void *d_temp_storage0 = NULL;
    size_t temp_storage_bytes0 = 0;

    // Determine temporary device storage requirements
    DeviceReduce::Sum(d_temp_storage0, temp_storage_bytes0, d_in, d_out, num_items);

    // Allocate temporary storage
    cudaMalloc(&d_temp_storage0, temp_storage_bytes0);

    // Launch kernel
    DeviceReduce::Sum(d_temp_storage0, temp_storage_bytes0, d_in, d_out, num_items);

    cudaFree(d_temp_storage0);
}

template <class RealType>
//void CudaKernel<RealType>::CubScan(thrust::device_vector<RealType>& d_in, thrust::device_vector<RealType>& d_out, int num_items)
void CudaKernel<RealType>::CubScan(RealType* d_in, RealType* d_out, int num_items)
{
    // Allocate temporary storage
    void *d_temp_storage0 = NULL;
    size_t temp_storage_bytes0 = 0;

    // Determine temporary device storage requirements
    //DeviceScan::InclusiveSum(d_temp_storage0, temp_storage_bytes0, thrust::raw_pointer_cast(&d_in[0]), thrust::raw_pointer_cast(&d_out[0]), num_items);
    DeviceScan::InclusiveSum(d_temp_storage0, temp_storage_bytes0, d_in, d_out, num_items);

    // Allocate temporary storage
    cudaMalloc(&d_temp_storage0, temp_storage_bytes0);

    // Launch kernel
    DeviceScan::InclusiveSum(d_temp_storage0, temp_storage_bytes0, d_in, d_out, num_items);

    cudaFree(d_temp_storage0);

}


/*
template <class RealType>
void CudaKernel<RealType>::computeAccDenomMalloc(int num_items)
{
    // Determine temporary device storage requirements
    DeviceScan::InclusiveSum(d_temp_storage, temp_storage_bytes, d_ExpXBeta, d_AccDenom, num_items);

    // Allocate temporary storage
    cudaMalloc(&d_temp_storage, temp_storage_bytes);
}

template <class RealType>
void CudaKernel<RealType>::computeAccDenom(int num_items)
{
    DeviceScan::InclusiveSum(d_temp_storage, temp_storage_bytes, d_ExpXBeta, d_AccDenom, num_items);
}

template <class RealType>
void CudaKernel<RealType>::computeAccNumerMalloc(int num_items)
{
    // Determine temporary device storage requirements
    DeviceScan::InclusiveSum(d_temp_storage, temp_storage_bytes, d_Numer, d_AccNumer, num_items);

    // Allocate temporary storage
    cudaMalloc(&d_temp_storage, temp_storage_bytes);
}

template <class RealType>
void CudaKernel<RealType>::computeAccNumer(int num_items)
{
    DeviceScan::InclusiveSum(d_temp_storage, temp_storage_bytes, d_Numer, d_AccNumer, num_items);
    DeviceScan::InclusiveSum(d_temp_storage, temp_storage_bytes, d_Numer2, d_AccNumer2, num_items);
}

template <class RealType>
void CudaKernel<RealType>::CubExpScanMalloc(int num_items)
{
    // Determine temporary device storage requirements
    DeviceScan::InclusiveSum(d_temp_storage, temp_storage_bytes, d_XBeta, d_AccDenom, num_items);

    // Allocate temporary storage
    cudaMalloc(&d_temp_storage, temp_storage_bytes);
}

template <class RealType>
void CudaKernel<RealType>::CubExpScan(int num_items)
{
//    auto start = std::chrono::steady_clock::now();

    TransformInputIterator<RealType, CustomExp, RealType*> d_itr(d_XBeta, exp_op);
    DeviceScan::InclusiveSum(d_temp_storage, temp_storage_bytes, d_itr, d_AccDenom, num_items);
    
//    auto end = std::chrono::steady_clock::now();
//    timerG += std::chrono::duration<double, std::milli>(end - start).count();
//    std::cout << "GPU takes " << timerG << " ms" << '\n';
}
*/

template class CudaKernel<float>;
template class CudaKernel<double>;
