// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System;

namespace System.Runtime.InteropServices
{
    /// <summary>
    /// Indicate that this assembly is generated by MCG
    /// </summary>
    [AttributeUsage(AttributeTargets.Assembly, AllowMultiple = false, Inherited = false)]
    public sealed class McgGeneratedAssemblyAttribute : Attribute
    {
        public McgGeneratedAssemblyAttribute()
        {
        }
    }
}