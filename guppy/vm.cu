#include <stdio.h>
#include <cuda.h>
#include <time.h>
#include <vector>

/*
#define BYTECODE_OP static inline __device__

BYTECODE_OP void load_slice() {

}

BYTECODE_OP void add(void* a, void *b) {

}
*/
double Now() {
    timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp.tv_sec + 1e-9 * tp.tv_nsec;
}

#define TIMEOP(op)\
{\
  double st = Now();\
  op;\
  double ed = Now();\
  fprintf(stderr, "%s finished in %.f seconds.\n", #op, end - start);\
}


enum OP_CODE {
  LOAD_SLICE, STORE_SLICE,    // load slice of global arrays into shared vector
  LOAD_SCALAR, STORE_SCALAR, // distribute scalar across elements of shared vector
  ADD, SUB, MUL, DIV,        // arithmetic between shared vectors
  BAD
};

struct Op {
  Op() : code(BAD), x(0), y(0), z(0) {}
  Op(OP_CODE code, int x, int y, int z) : code(code), x(x), y(y), z(z)  {}

  OP_CODE code; 
  int x, y, z;
};

struct Program {
	std::vector<Op> _ops;
	Op* _gpu_ptr;


	Program& Add(int x, int y, int z) {
		_ops.push_back(Op(ADD, x, y, z));
		return *this;
	}
	Program& LoadSlice(int src, int dst) {
		_ops.push_back(Op(LOAD_SLICE, src, dst, 0));
		return *this;
	}
	Program& StoreSlice(int src, int dst) {
		_ops.push_back(Op(LOAD_SLICE, src, dst, 0));
		return *this;
	}

	int size() {
      return _ops.size();
	}

	int nbytes () {
	  return sizeof(Op) * this->size();
	}

	Op* host_ptr() {
	  return &_ops[0];
	}
	Op* to_gpu() {
	  if (_gpu_ptr) {
		  return _gpu_ptr;
	  }
	  cudaMalloc(&_gpu_ptr, this->nbytes());
	  cudaMemcpy(_gpu_ptr, this->host_ptr(), this->nbytes(), cudaMemcpyHostToDevice);
	  return _gpu_ptr;
	}

	Program() : _gpu_ptr(NULL) {}
	~Program () {
	  if (_gpu_ptr) {
	    cudaFree(_gpu_ptr);
	  }
	}
};


struct Vec {
  int _n;
  int _nbytes;
  float* _host_data;
  float* _gpu_data;
  bool _host_dirty;
  bool _gpu_dirty;

  void init(int n) {
    _n = n;
    _nbytes = sizeof(float) * n;
    _host_data = new float[n];
	cudaMalloc(&_gpu_data, this->_nbytes);
    _host_dirty = false;
    _gpu_dirty = true;
  }

  Vec(int n) {
    this->init(n);
  }

  Vec (int n, float fill_value) {
    this->init(n);
    for (int i = 0; i < n; ++i) {
      _host_data[i] = fill_value;
    }
  }

  float* get_gpu_data() {
     if (_gpu_dirty) { this->copy_to_gpu(); }
     _host_dirty = true;
     _gpu_dirty = false;
     return _gpu_data;
  }

  float* get_host_data() {
	  if (_host_dirty) { this->copy_to_host();}
	  _gpu_dirty = true;
	  _host_dirty = false;
	  return _host_data;
  }

  void copy_to_host() {
	  cudaMemcpy(this->_host_data, this->_gpu_data, this->_nbytes, cudaMemcpyDeviceToHost);
  }


  void copy_to_gpu() {
	  cudaMemcpy(this->_gpu_data, this->_host_data, this->_nbytes, cudaMemcpyHostToDevice);
  }

  ~Vec() {
	  cudaFree(this->_gpu_data);
	  delete[] this->_host_data;
  }

};

#define REGISTER_WIDTH 128
#define NUM_REGISTERS 16

__global__ void run(
		Op* program, int n_ops,
		float** values, int n_args,
		float* constants, int n_consts) {
  int startIdx = blockIdx.x * blockDim.x; 
  // int stopIdx = startIdx + blockDim.x;
  __shared__ float registers[NUM_REGISTERS][REGISTER_WIDTH];

  for (int pc = 0; pc < n_ops; ++pc) {
    Op op = program[pc];
    switch (op.code) {
    case LOAD_SLICE: {
      float* dst = registers[op.y] + threadIdx.x;
      float* src = values[op.x] + startIdx + threadIdx.x;
      *dst = *src;
    }
    break;

    case STORE_SLICE: {
      float* dst = values[op.y] + startIdx + threadIdx.x;
      float* src = registers[op.x] + threadIdx.x;
      *dst = *src;
    }
    break;

    case LOAD_SCALAR: {

    }
    break;

    case STORE_SCALAR: {

    }
    break;

	case ADD: {
	    float* x = registers[op.x] + threadIdx.x; //+ startIdx + threadIdx.x;
	    float* y = registers[op.y] + threadIdx.x; //+ startIdx + threadIdx.x;
	    float* z = values[op.z] + threadIdx.x; //+ startIdx + threadIdx.x;
        *z = *x + *y;
      }
	break;
    }  
  }
}

#define THREADS_PER_BLOCK 512

int main(int argc, const char** argv) { 
  int N = 400 * THREADS_PER_BLOCK;
  if (argc > 1) {
    N = strtol(argv[1], NULL, 10);
  }
    
  Vec a(N, 1.0);
  Vec b(N, 2.0);
  Vec c(N);
  
  const int n_values = 3;
  float* h_values[n_values];
  h_values[0]= a.get_gpu_data();
  h_values[1] = b.get_gpu_data();
  h_values[2] = c.get_gpu_data();

  float** d_values;
  cudaMalloc(&d_values, sizeof(float*) * n_values);
  cudaMemcpy(d_values, h_values, sizeof(float*) * n_values, cudaMemcpyHostToDevice);


  Program h_program;

  h_program.
    LoadSlice(0,0).
    LoadSlice(1,1).
    Add(0,1,2).
    StoreSlice(2,2);

  double st = Now();
  run<<<N / THREADS_PER_BLOCK, THREADS_PER_BLOCK>>>(
		  h_program.to_gpu(), h_program.size(),
		  d_values, n_values,
		  0, 0);
  cudaDeviceSynchronize();
  double ed = Now();
  fprintf(stderr, "%.5f seconds\n", ed -st);

  float* ad = a.get_host_data();
  printf("%f %f %f\n", ad[0], ad[1], ad[2]);
  float* cd = c.get_host_data();
  printf("%f %f %f\n", cd[0], cd[1], cd[2]);
  return 0; 
}
