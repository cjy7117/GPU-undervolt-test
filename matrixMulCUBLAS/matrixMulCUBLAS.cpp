////////////////////////////////////////////////////////////////////////////
//
// Copyright 1993-2015 NVIDIA Corporation.  All rights reserved.
//
// Please refer to the NVIDIA end user license agreement (EULA) associated
// with this source code for terms and conditions that govern your use of
// this software. Any use, reproduction, disclosure, or distribution of
// this software and related documentation outside the terms of the EULA
// is strictly prohibited.
//
////////////////////////////////////////////////////////////////////////////

//
// Matrix multiplication: C = A * B.
// Host code.
//
// This sample implements matrix multiplication as described in Chapter 3
// of the programming guide and uses the CUBLAS library to demonstrate
// the best performance.

// SOME PRECAUTIONS:
// IF WE WANT TO CALCULATE ROW-MAJOR MATRIX MULTIPLY C = A * B,
// WE JUST NEED CALL CUBLAS API IN A REVERSE ORDER: cublasSegemm(B, A)!
// The reason is explained as follows:

// CUBLAS library uses column-major storage, but C/C++ use row-major storage.
// When passing the matrix pointer to CUBLAS, the memory layout alters from
// row-major to column-major, which is equivalent to an implicit transpose.

// In the case of row-major C/C++ matrix A, B, and a simple matrix multiplication
// C = A * B, we can't use the input order like cublasSgemm(A, B)  because of
// implicit transpose. The actual result of cublasSegemm(A, B) is A(T) * B(T).
// If col(A(T)) != row(B(T)), equal to row(A) != col(B), A(T) and B(T) are not
// multipliable. Moreover, even if A(T) and B(T) are multipliable, the result C
// is a column-based cublas matrix, which means C(T) in C/C++, we need extra
// transpose code to convert it to a row-based C/C++ matrix.

// To solve the problem, let's consider our desired result C, a row-major matrix.
// In cublas format, it is C(T) actually (because of the implicit transpose).
// C = A * B, so C(T) = (A * B) (T) = B(T) * A(T). Cublas matrice B(T) and A(T)
// happen to be C/C++ matrice B and A (still because of the implicit transpose)!
// We don't need extra transpose code, we only need alter the input order!
//
// CUBLAS provides high-performance matrix multiplication.
// See also:
// V. Volkov and J. Demmel, "Benchmarking GPUs to tune dense linear algebra,"
// in Proc. 2008 ACM/IEEE Conf. on Supercomputing (SC '08),
// Piscataway, NJ: IEEE Press, 2008, pp. Art. 31:1-11.
//

// Utilities and system includes
#include <assert.h>
#include <helper_string.h>  // helper for shared functions common to CUDA Samples

// CUDA runtime
#include <cuda_runtime.h>
#include <cublas_v2.h>

// CUDA and CUBLAS functions
#include <helper_functions.h>
#include <helper_cuda.h>

#include <nvml.h>
using std::cout;
using std::endl;

#ifndef min
#define min(a,b) ((a < b) ? a : b)
#endif
#ifndef max
#define max(a,b) ((a > b) ? a : b)
#endif

typedef struct _matrixSize      // Optional Command-line multiplier for matrix sizes
{
    unsigned int uiWA, uiHA, uiWB, uiHB, uiWC, uiHC;
} sMatrixSize;


void undervolte()
{
    if (nvmlInit () != NVML_SUCCESS)
    {
        cout << "init error";
        return;
    }
    int i = 0;
    nvmlReturn_t result;
    nvmlDevice_t device;
    result = nvmlDeviceGetHandleByIndex(i, &device);
    if (NVML_SUCCESS != result)
    {
      printf("Failed to get handle for device %i: %s\n", i, nvmlErrorString(result));
      return;
    }

    result = nvmlDeviceSetPowerManagementLimit (device, 30000);
    if (NVML_SUCCESS != result)
    {
      printf("Failed to set power limit of device %i: %s\n", i, nvmlErrorString(result));
      return;
    }

    result = nvmlDeviceSetApplicationsClocks ( device, 3510, 1885 );
    if (NVML_SUCCESS != result)
    {
      printf("Failed to set clock of device %i: %s\n", i, nvmlErrorString(result));
      return;
    }
    nvmlEnableState_t set = NVML_FEATURE_DISABLED;
    result = nvmlDeviceSetAutoBoostedClocksEnabled(device, set);
    if (NVML_SUCCESS != result)
    {
      printf("Failed to disable autoboost of device %i: %s\n", i, nvmlErrorString(result));
      return;
    }

}

void resetvolte()
{
    if (nvmlInit () != NVML_SUCCESS)
    {
        cout << "init error";
        return;
    }
    int i = 0;
    nvmlReturn_t result;
    nvmlDevice_t device;
    result = nvmlDeviceGetHandleByIndex(i, &device);
    if (NVML_SUCCESS != result)
    {
      printf("Failed to get handle for device %i: %s\n", i, nvmlErrorString(result));
      return;
    }
    
    result = nvmlDeviceSetPowerManagementLimit (device, 38500);
    if (NVML_SUCCESS != result)
    {
      printf("Failed to set power limit of device %i: %s\n", i, nvmlErrorString(result));
      return;
    }

    result = nvmlDeviceResetApplicationsClocks (device);
    if (NVML_SUCCESS != result)
    {
      printf("Failed to reset clock of device %i: %s\n", i, nvmlErrorString(result));
      return;
    }

    nvmlEnableState_t set = NVML_FEATURE_ENABLED;
    result = nvmlDeviceSetAutoBoostedClocksEnabled(device, set);
    if (NVML_SUCCESS != result)
    {
      printf("Failed to disable autoboost of device %i: %s\n", i, nvmlErrorString(result));
      return;
    }

}

////////////////////////////////////////////////////////////////////////////////
//! Compute reference data set matrix multiply on CPU
//! C = A * B
//! @param C          reference data, computed but preallocated
//! @param A          matrix A as provided to device
//! @param B          matrix B as provided to device
//! @param hA         height of matrix A
//! @param wB         width of matrix B
////////////////////////////////////////////////////////////////////////////////
void
matrixMulCPU(float *C, const float *A, const float *B, unsigned int hA, unsigned int wA, unsigned int wB)
{
    for (unsigned int i = 0; i < hA; ++i)
        for (unsigned int j = 0; j < wB; ++j)
        {
            double sum = 0;

            for (unsigned int k = 0; k < wA; ++k)
            {
                double a = A[i * wA + k];
                double b = B[k * wB + j];
                sum += a * b;
            }

            C[i * wB + j] = (float)sum;
        }
}

// Allocates a matrix with random float entries.
void randomInit(float *data, int size)
{
    for (int i = 0; i < size; ++i)
        data[i] = rand() / (float)RAND_MAX;
}

void printDiff(float *data1, float *data2, int width, int height, int iListLength, float fListTol)
{
    printf("Listing first %d Differences > %.6f...\n", iListLength, fListTol);
    int i,j,k;
    int error_count=0;

    for (j = 0; j < height; j++)
    {
        if (error_count < iListLength)
        {
            printf("\n  Row %d:\n", j);
        }

        for (i = 0; i < width; i++)
        {
            k = j * width + i;
            float fDiff = fabs(data1[k] - data2[k]);

            if (fDiff > fListTol)
            {
                if (error_count < iListLength)
                {
                    printf("    Loc(%d,%d)\tCPU=%.5f\tGPU=%.5f\tDiff=%.6f\n", i, j, data1[k], data2[k], fDiff);
                }

                error_count++;
            }
        }
    }

    printf(" \n  Total Errors = %d\n", error_count);
}



////////////////////////////////////////////////////////////////////////////////
//! Run a simple test matrix multiply using CUBLAS
////////////////////////////////////////////////////////////////////////////////
int matrixMultiply()
{
    sMatrixSize matrix_size;
    int n = 10240;

    matrix_size.uiWA = n;
    matrix_size.uiHA = n;
    matrix_size.uiWB = n;
    matrix_size.uiHB = n;
    matrix_size.uiWC = n;
    matrix_size.uiHC = n;

    // allocate host memory for matrices A and B
    unsigned int size_A = matrix_size.uiWA * matrix_size.uiHA;
    unsigned int mem_size_A = sizeof(float) * size_A;
    float *h_A = (float *)malloc(mem_size_A);

    unsigned int size_B = matrix_size.uiWB * matrix_size.uiHB;
    unsigned int mem_size_B = sizeof(float) * size_B;
    float *h_B = (float *)malloc(mem_size_B);

    unsigned int size_C = matrix_size.uiWC * matrix_size.uiHC;
    unsigned int mem_size_C = sizeof(float) * size_C;
    float *h_C      = (float *) malloc(mem_size_C);
    float *h_C2      = (float *) malloc(mem_size_C);

    float *d_A, *d_B, *d_C, *d_C2;
    checkCudaErrors(cudaMalloc((void **) &d_A, mem_size_A));
    checkCudaErrors(cudaMalloc((void **) &d_B, mem_size_B));
    checkCudaErrors(cudaMalloc((void **) &d_C, mem_size_C));
    checkCudaErrors(cudaMalloc((void **) &d_C2, mem_size_C));

    // set seed for rand()
    srand(2006);

    // initialize host memory
    randomInit(h_A, size_A);
    randomInit(h_B, size_B);
    checkCudaErrors(cudaMemcpy(d_A, h_A, mem_size_A, cudaMemcpyHostToDevice));
    checkCudaErrors(cudaMemcpy(d_B, h_B, mem_size_B, cudaMemcpyHostToDevice));

    const float alpha = 1.0f;
    const float beta  = 0.0f;
    cublasHandle_t handle;
    cudaEvent_t start, stop;
    checkCudaErrors(cudaEventCreate(&start));
    checkCudaErrors(cudaEventCreate(&stop));

    checkCudaErrors(cublasCreate(&handle));


    printf("Computing result using CUBLAS (normal power)...\n");
    // resetvolte();
    checkCudaErrors(cudaEventRecord(start, NULL));
    checkCudaErrors(cublasSgemm(handle, 
                                CUBLAS_OP_N, CUBLAS_OP_N, 
                                matrix_size.uiWB, matrix_size.uiHA, matrix_size.uiWA, 
                                &alpha, 
                                d_B, matrix_size.uiWB, 
                                d_A, matrix_size.uiWA, 
                                &beta, 
                                d_C2, matrix_size.uiWB));
    checkCudaErrors(cudaEventRecord(stop, NULL));
    checkCudaErrors(cudaEventSynchronize(stop));
    float msecTotal = 0.0f;
    checkCudaErrors(cudaEventElapsedTime(&msecTotal, start, stop));
    //Compute and print the performance
    float msecPerMatrixMul = msecTotal;
    double flopsPerMatrixMul = 2.0 * (double)matrix_size.uiHC * (double)matrix_size.uiWC * (double)matrix_size.uiHB;
    double gigaFlops = (flopsPerMatrixMul * 1.0e-9f) / (msecPerMatrixMul / 1000.0f);
    printf(
        "Performance= %.2f GFlop/s, Time= %.3f msec, Size= %.0f Ops\n",
	
        gigaFlops,
        msecPerMatrixMul,
        flopsPerMatrixMul);


    checkCudaErrors(cudaMemcpy(h_C2, d_C2, mem_size_C, cudaMemcpyDeviceToHost));

   
    // create and start timer
    printf("Computing result using CUBLAS (low power)...\n");
    //undervolte();
    // execute the kernel
    int nIter = 100;

    // Record the start event
    //checkCudaErrors(cudaEventRecord(start, NULL));
    int fail_count = 0;
    float total_perf = 0.0;
    for (int j = 0; j < nIter; j++)
    {
        //note cublas is column primary!
        //need to transpose the order
        checkCudaErrors(cudaEventRecord(start, NULL));
        checkCudaErrors(cublasSgemm(handle, 
                                    CUBLAS_OP_N, CUBLAS_OP_N, 
                                    matrix_size.uiWB, matrix_size.uiHA, matrix_size.uiWA, 
                                    &alpha, 
                                    d_B, matrix_size.uiWB, 
                                    d_A, matrix_size.uiWA, 
                                    &beta,
                                    d_C, matrix_size.uiWB));
        checkCudaErrors(cudaEventRecord(stop, NULL));
        checkCudaErrors(cudaEventSynchronize(stop));
        float msecTotal = 0.0f;
        checkCudaErrors(cudaEventElapsedTime(&msecTotal, start, stop));
        //Compute and print the performance
        float msecPerMatrixMul = msecTotal;
        double flopsPerMatrixMul = 2.0 * (double)matrix_size.uiHC * (double)matrix_size.uiWC * (double)matrix_size.uiHB;
        double gigaFlops = (flopsPerMatrixMul * 1.0e-9f) / (msecPerMatrixMul / 1000.0f);
        printf(
            "[%d]Performance= %.2f GFlop/s, Time= %.3f msec, Size= %.0f Ops\n",
	    j,
            gigaFlops,
            msecPerMatrixMul,
            flopsPerMatrixMul);
	total_perf += gigaFlops;

        // copy result from device to host
        checkCudaErrors(cudaMemcpy(h_C, d_C, mem_size_C, cudaMemcpyDeviceToHost));
        // check result (CUBLAS)
        bool resCUBLAS = sdkCompareL2fe(h_C2, h_C, size_C, 1.0e-10f);

        if (resCUBLAS != true)
        {
            printDiff(h_C2, h_C, matrix_size.uiWC, matrix_size.uiHC, 100, 1.0e-5f);\
	    fail_count++;
        }

        printf("Comparing CUBLAS Matrix Multiply with CPU results: %s\n", (true == resCUBLAS) ? "PASS" : "FAIL");

    }
	printf("total test: %d, failed: %d.\n", nIter, fail_count);
	printf("failure rate: %f.\n", (float)fail_count/nIter);
	printf("average perf: %.2f.\n", total_perf/nIter);
        // printf("done.\n");

        // // Record the stop event
        // checkCudaErrors(cudaEventRecord(stop, NULL));

        // // Wait for the stop event to complete
        // checkCudaErrors(cudaEventSynchronize(stop));

        // float msecTotal = 0.0f;
        // checkCudaErrors(cudaEventElapsedTime(&msecTotal, start, stop));

        // // Compute and print the performance
        // float msecPerMatrixMul = msecTotal / nIter;
        // double flopsPerMatrixMul = 2.0 * (double)matrix_size.uiHC * (double)matrix_size.uiWC * (double)matrix_size.uiHB;
        // double gigaFlops = (flopsPerMatrixMul * 1.0e-9f) / (msecPerMatrixMul / 1000.0f);
        // printf(
        //     "Performance= %.2f GFlop/s, Time= %.3f msec, Size= %.0f Ops\n",
        //     gigaFlops,
        //     msecPerMatrixMul,
        //     flopsPerMatrixMul);

        // copy result from device to host
        //checkCudaErrors(cudaMemcpy(h_CUBLAS, d_C, mem_size_C, cudaMemcpyDeviceToHost));

        // Destroy the handle
        checkCudaErrors(cublasDestroy(handle));

    

    

    //printf("\nNOTE: The CUDA Samples are not meant for performance measurements. Results may vary when GPU Boost is enabled.\n");

    // clean up memory
    free(h_A);
    free(h_B);
    free(h_C);
    free(h_C2);
    checkCudaErrors(cudaFree(d_A));
    checkCudaErrors(cudaFree(d_B));
    checkCudaErrors(cudaFree(d_C));
    checkCudaErrors(cudaFree(d_C2));

    // if (resCUBLAS == true)
    // {
    //     return EXIT_SUCCESS;    // return value = 1
    // }
    // else
    // {
    //     return EXIT_FAILURE;     // return value = 0
    // }
    return 0;
}



////////////////////////////////////////////////////////////////////////////////
// Program main
////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv)
{
    printf("[Matrix Multiply CUBLAS] - Starting...\n");

    int devID = 0, sizeMult = 5;
    sMatrixSize matrix_size;


    

    int matrix_result = matrixMultiply();
    //resetvolte();
    return matrix_result;
}
