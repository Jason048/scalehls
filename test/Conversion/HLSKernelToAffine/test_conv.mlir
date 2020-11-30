// RUN: scalehls-opt -hlskernel-to-affine %s | FileCheck %s

// CHECK: module {
func @test_conv(%I: memref<10x3x32x32xf32>, %K: memref<16x3x5x5xf32>, %B: memref<16xf32>, %O: memref<10x16x28x28xf32>) -> () {
  "hlskernel.conv" (%I, %K, %B, %O) {} : (memref<10x3x32x32xf32>, memref<16x3x5x5xf32>, memref<16xf32>, memref<10x16x28x28xf32>) -> ()
  return
}