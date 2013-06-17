#ifndef GUPPY_VM_H
#define GUPPY_VM_H

#include <stdint.h> 

#include "config.h"
#include "bytecode.h"



__device__ inline void run_local_instruction(
    Instruction* instr, 
    int local_idx, 
    float** arrays,
    float** vectors, 
    int32_t* int_scalars, 
    float* float_scalars,  
    const float* constants) {
  switch(instr->tag) {
    case Add::code: {
      Add* op = (Add*) instr; 
      float a = float_scalars[op->arg1];
      float b = float_scalars[op->arg2];
      float_scalars[op->result] = a+b;
      break;  
    }
  }  
}
                               
__device__ inline void run_subprogram() {}

#define CALL_EVAL(t) \
  ((t*)instr)->eval(local_idx, \
           arrays,\
           array_lengths,\
           (float**) vectors, \
           int_scalars, \
           long_scalars, \
           float_scalars, \
           double_scalars)

__global__ void run(char* program,
                    long program_nbytes,
                    float** arrays,
                    const size_t* array_lengths) {
  const int block_offset = blockIdx.y * gridDim.x + blockIdx.x;
  const int local_idx = threadIdx.y * blockDim.x + threadIdx.x;
  const int local_vector_offset = local_idx * kOpsPerThread; 
  const int next_vector_offset = local_vector_offset + kOpsPerThread; 


  __shared__ float vectors[kNumVecRegisters][kVectorWidth+kVectorPadding];
  #if SCALAR_REGISTERS_SHARED
    __shared__ int32_t int_scalars[kNumIntRegisters];
    __shared__ int64_t long_scalars[kNumIntRegisters]; 
    __shared__ float   float_scalars[kNumFloatRegisters];
    __shared__ double  double_scalars[kNumFloatRegisters]; 
  #else
    int32_t int_scalars[kNumIntRegisters];
    int64_t long_scalars[kNumLongRegisters];
    float   float_scalars[kNumFloatRegisters];
    double  double_scalars[kNumFloatRegisters]; 
  #endif
  

  int_scalars[BlockStart] = block_offset; 
  int_scalars[VecWidth] = kVectorWidth;
  int_scalars[BlockEltStart] = block_offset * kVectorWidth; 

  #if PREFETCH_GPU_BYTECODE 
    /* preload program so that we don't make 
       repeated global memory requests 
    */  
    __shared__ char  cached_program[kMaxProgramLength];
    for (int i = local_idx; i < program_nbytes; i+=kThreadsPerBlock) {
      cached_program[i] = program[i];      
    }  
  #endif 

  int pc = 0;
  Instruction* instr;
  while (pc < program_nbytes) {
    
    #if PREFETCH_GPU_BYTECODE 
      instr = (Instruction*) &cached_program[pc];
    #else
      instr = (Instruction*) &program[pc]; 
    #endif 
    pc += instr->size;

    switch (instr->tag) {
    case LoadVector::code: {
      LoadVector* load_slice = (LoadVector*) instr;
      float* reg = vectors[load_slice->target_vector]; 
      const float* src = arrays[load_slice->source_array];
      const int start = int_scalars[load_slice->start_idx];
      int nelts = int_scalars[load_slice->nelts];
      #if VECTOR_LOAD_CHECK_BOUNDS
        nelts = nelts <= kVectorWidth ? nelts : kVectorWidth; 
      #endif
       
      #pragma unroll 5 
      for ( int i = local_idx * kOpsPerThread; i < (local_idx+1) * kOpsPerThread; ++i) { 
          reg[i] = src[start+i];
      }
       
      break;
    }
    
    case LoadVector2::code: {
      LoadVector2* load_slice = (LoadVector2*) instr;
      
      float* reg1 = vectors[load_slice->target_vector1]; 
      const float* src1 = arrays[load_slice->source_array1];
      
      float* reg2 = vectors[load_slice->target_vector2]; 
      const float* src2 = arrays[load_slice->source_array2];
      
      const int start = int_scalars[load_slice->start_idx];
      int nelts = int_scalars[load_slice->nelts];
      #if VECTOR_LOAD_CHECK_BOUNDS
        nelts = nelts <= kVectorWidth ? nelts : kVectorWidth; 
      #endif 
         
      #pragma unroll 5
      for ( int i = local_idx * kOpsPerThread; i < (local_idx+1) * kOpsPerThread; ++i) { 
      //for (int i = local_idx * kOpsPerThread; i < (local_idx+1)*kOpsPerThread; ++i) {
      //MEMORY_ACCESS_LOOP { 
          reg1[i] = src1[start+i];
          reg2[i] = src2[start+i];
      }
       
      break;
    }

    case StoreVector::code: {
      StoreVector* store = (StoreVector*) instr;
      const float* reg = vectors[store->source_vector];
      float* dst = arrays[store->target_array];
      const int start = int_scalars[store->start_idx];
      int nelts = int_scalars[store->nelts];
      #if VECTOR_STORE_CHECK_BOUNDS
        nelts = nelts <= kVectorWidth ? nelts : kVectorWidth; 
      #endif 
      
      
      #pragma unroll 5
      for ( int i = local_idx * kOpsPerThread; i < (local_idx+1) * kOpsPerThread; ++i) { 
      //int i = local_idx * kOpsPerThread; i < (local_idx+1)*kOpsPerThread; ++i) {   
      //MEMORY_ACCESS_LOOP {
          dst[i+start] = reg[i]; 
      }
      break;
    }

    case Add::code: {
      Add* op = (Add*) instr; 
      float* a = vectors[op->arg1];
      float* b = vectors[op->arg2];
      float* c = vectors[op->result];
      #pragma unroll 5
      for ( int i = local_idx * kOpsPerThread; i < (local_idx+1) * kOpsPerThread; ++i) { 
        c[i] = a[i] + b[i];   
      } 
      break;

    }
 
    case IAdd::code: {
      IAdd* op = (IAdd*) instr; 
      const float* a = vectors[op->arg];
      float* b = vectors[op->result];
     
      #pragma unroll 5
      for (int i = local_idx * kOpsPerThread; i < (local_idx+1)*kOpsPerThread; ++i) {  
        b[i] += a[i]; 
      }
      break;
    }
      
    case Map::code: {
      Map* op = (Map*) instr;
      const float* src = vectors[op->source_vector];
      float* dst = vectors[op->target_vector];   
      float* in_reg = &float_scalars[op->input_elt_reg]; 
      float* out_reg = &float_scalars[op->output_elt_reg]; 

      for (int i = local_idx * kOpsPerThread; i < (local_idx+1)*kOpsPerThread; ++i) {
        in_reg[0] = src[i];   
        run_subprogram();   
      } 
      break;
    }

    case Map2::code: {
      Map2* op = (Map2*) instr;
      const float* src1 = vectors[op->source_vector1];
      const float* src2 = vectors[op->source_vector2]; 
      float* dst = vectors[op->target_vector];   
      float* in_reg1 = &float_scalars[op->input_elt_reg1];
      float* in_reg2 = &float_scalars[op->input_elt_reg2];  
      float* out_reg = &float_scalars[op->output_elt_reg]; 
      

      for (int i = local_idx * kOpsPerThread; i < (local_idx+1)*kOpsPerThread; ++i) {
        in_reg1[0] = src1[i];   
        in_reg2[0] = src2[i]; 

        run_subprogram();   
      } 
      break;
    } 
    } // switch
  } // while 
} // run 

#endif 
