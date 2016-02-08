// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

.assembly extern System.Runtime
{
  .publickeytoken = (B0 3F 5F 7F 11 D5 0A 3A )
  .ver 4:0:0:0
}
.assembly extern System.Console
{
  .publickeytoken = (B0 3F 5F 7F 11 D5 0A 3A )
  .ver 4:0:0:0
}
.assembly extern A
{
  .publickeytoken = (19 B5 AE EE 5E 97 C0 D9 )
  .ver 0:0:0:0
}
.assembly extern A as A2
{
  .publickeytoken = (D2 DB A5 87 5B EA E2 CF )
  .ver 0:0:0:0
}
.assembly Collision
{
  .hash algorithm 0x00008004
  .ver 0:0:0:0
}
.module Collision.exe
.imagebase 0x00400000
.file alignment 0x00000200
.stackreserve 0x00100000
.subsystem 0x0003       // WINDOWS_CUI
.corflags 0x00000001    //  ILONLY

.class private auto ansi beforefieldinit Hello.Program extends [System.Runtime]System.Object
{
  .method private hidebysig static void  Main(string[] args) cil managed
  {
    .entrypoint
    .maxstack  8
    ldstr      "Hello world "
    call       void [System.Console]System.Console::Write(string)
    call       string [A]A.A::Val()
    call       void [System.Console]System.Console::Write(string)
    ldstr      " "
    call       void [System.Console]System.Console::Write(string)
    call       string [A2]A.A::Val()
    call       void [System.Console]System.Console::WriteLine(string)
    ret
  }

  .method public hidebysig specialname rtspecialname instance void  .ctor() cil managed
  {
    .maxstack  8
    ldarg.0
    call       instance void [System.Runtime]System.Object::.ctor()
    ret
  }
}
