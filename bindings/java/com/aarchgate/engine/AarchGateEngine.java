package com.aarchgate.engine;

import java.nio.ByteBuffer;

public class AarchGateEngine {
    private long handle;

    // Load native library
    static {
        System.loadLibrary("aarchgate_java");
    }

    public AarchGateEngine() {
        this.handle = nativeCreate();
        if (this.handle == 0) {
            throw new RuntimeException("Failed to create AarchGate Engine");
        }
    }

    public void destroy() {
        if (this.handle != 0) {
            nativeDestroy(this.handle);
            this.handle = 0;
        }
    }

    public void registerSchema(String name, FieldDescriptor[] fields, long stride) {
        if (nativeRegisterSchema(this.handle, name, fields, stride) != 0) {
            throw new RuntimeException("Failed to register schema");
        }
    }

    public void setLogic(String schemaName, long irRootPtr, int mode) {
        if (nativeSetLogic(this.handle, schemaName, irRootPtr, mode) != 0) {
            throw new RuntimeException("Failed to set logic");
        }
    }

    public long execute(ByteBuffer data, long count) {
        if (!data.isDirect()) {
            throw new IllegalArgumentException("Data buffer must be a direct ByteBuffer for zero-copy JIT ingestion");
        }
        long result = nativeExecute(this.handle, data, count);
        if (result == -1) {
            throw new RuntimeException("Execution failed");
        }
        return result;
    }

    public static class FieldDescriptor {
        public String name;
        public long offset;
        public long bitWidth;
        public int dataType;

        public FieldDescriptor(String name, long offset, long bitWidth, int dataType) {
            this.name = name;
            this.offset = offset;
            this.bitWidth = bitWidth;
            this.dataType = dataType;
        }
    }

    public static long createUniversalTestLogic() {
        return nativeCreateUniversalTestLogic();
    }

    // Native JNI functions
    private static native long nativeCreateUniversalTestLogic();
    private native long nativeCreate();
    private native void nativeDestroy(long handle);
    private native int nativeRegisterSchema(long handle, String name, FieldDescriptor[] fields, long stride);
    private native int nativeSetLogic(long handle, String schemaName, long irRootPtr, int mode);
    private native long nativeExecute(long handle, ByteBuffer data, long count);
}
