; RUN: llc -mtriple=hexagon-unknown-elf -mcpu=hexagonv67 < %s | FileCheck %s


; CHECK-LABEL: t1
; CHECK: dfmax

define dso_local double @t1(double %a, double %b) local_unnamed_addr {
entry:
  %0 = tail call double @llvm.maxnum.f64(double %a, double %b)
  ret double %0
}

; CHECK-LABEL: t2
; CHECK: dfmin

define dso_local double @t2(double %a, double %b) local_unnamed_addr {
entry:
  %0 = tail call double @llvm.minnum.f64(double %a, double %b)
  ret double %0
}

; CHECK-LABEL: t3
; CHECK: sfmax

define dso_local float @t3(float %a, float %b) local_unnamed_addr {
entry:
  %0 = tail call float @llvm.maxnum.f32(float %a, float %b)
  ret float %0
}

; CHECK-LABEL: t4
; CHECK: sfmin

define dso_local float @t4(float %a, float %b) local_unnamed_addr {
entry:
  %0 = tail call float @llvm.minnum.f32(float %a, float %b)
  ret float %0
}

; CHECK-LABEL: t5
; CHECK: dfmax

define dso_local double @t5(double %a, double %b) local_unnamed_addr {
entry:
  %0 = tail call double @llvm.maximumnum.f64(double %a, double %b)
  ret double %0
}

; CHECK-LABEL: t6
; CHECK: dfmin

define dso_local double @t6(double %a, double %b) local_unnamed_addr {
entry:
  %0 = tail call double @llvm.minimumnum.f64(double %a, double %b)
  ret double %0
}

; CHECK-LABEL: t7
; CHECK: sfmax

define dso_local float @t7(float %a, float %b) local_unnamed_addr {
entry:
  %0 = tail call float @llvm.maximumnum.f32(float %a, float %b)
  ret float %0
}

; CHECK-LABEL: t8
; CHECK: sfmin

define dso_local float @t8(float %a, float %b) local_unnamed_addr {
entry:
  %0 = tail call float @llvm.minimumnum.f32(float %a, float %b)
  ret float %0
}

declare double @llvm.minnum.f64(double, double) #1
declare double @llvm.maxnum.f64(double, double) #1
declare float @llvm.maxnum.f32(float, float) #1
declare float @llvm.minnum.f32(float, float) #1
declare double @llvm.minimumnum.f64(double, double) #1
declare double @llvm.maximumnum.f64(double, double) #1
declare float @llvm.maximumnum.f32(float, float) #1
declare float @llvm.minimumnum.f32(float, float) #1

attributes #1 = { nounwind readnone speculatable }
