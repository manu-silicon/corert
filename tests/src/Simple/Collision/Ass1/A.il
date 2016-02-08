// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

.assembly extern System.Runtime
{
  .publickeytoken = (B0 3F 5F 7F 11 D5 0A 3A)
  .ver 4:0:0:0
}
.assembly A
{
  .hash algorithm 0x00008004
  .ver 0:0:0:0
}
.module A.dll
.imagebase 0x10000000
.file alignment 0x00000200
.stackreserve 0x00100000
.subsystem 0x0003
.corflags 0x00000001

.class public abstract auto ansi sealed beforefieldinit A.A extends [System.Runtime]System.Object
{
  .method public hidebysig static string Val() cil managed
  {
    .maxstack  1
    ldstr      "Assembly 1"
    ret
  }
}
