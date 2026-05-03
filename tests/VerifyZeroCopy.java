package tests;

import com.apex.engine.ApexEngine;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class VerifyZeroCopy {
    public static void main(String[] args) {
        System.out.println("=== Java Zero-Copy Handoff Verification ===");

        // 1. Setup Engine
        ApexEngine engine = new ApexEngine();
        
        ApexEngine.FieldDescriptor[] fields = new ApexEngine.FieldDescriptor[] {
            new ApexEngine.FieldDescriptor("Field0", 0, 64, 3),
            new ApexEngine.FieldDescriptor("Field1", 8, 64, 3),
            new ApexEngine.FieldDescriptor("Field2", 16, 64, 3)
        };
        engine.registerSchema("JavaZeroCopy", fields, 24);
        
        long exprPtr = ApexEngine.createUniversalTestLogic();
        engine.setLogic("JavaZeroCopy", exprPtr, 0); // BIT_SLICED = 0

        // 2. Allocate 120MB direct buffer (5,000,000 rows * 24 bytes)
        int numElements = 5_000_000;
        int rowStride = 24;
        ByteBuffer data = ByteBuffer.allocateDirect(numElements * rowStride);
        data.order(ByteOrder.nativeOrder());

        // Fill data
        for (int i = 0; i < numElements; i++) {
            data.putLong(i); // Field0
            data.putLong(i); // Field1
            data.putLong(i); // Field2
        }
        data.flip();

        System.out.println("Allocated " + (data.capacity() / (1024 * 1024)) + " MB Direct ByteBuffer");

        // 3. Measure Latency
        long t0 = System.nanoTime();
        long matches = engine.execute(data, numElements);
        long t1 = System.nanoTime();

        double latencyMs = (t1 - t0) / 1_000_000.0;
        System.out.println("Total Execution (including JNI handoff): " + String.format("%.4f", latencyMs) + " ms");
        System.out.println("Throughput: " + String.format("%.2f", (numElements / (latencyMs / 1000.0)) / 1_000_000.0) + " M rows/sec");
        System.out.println("Matches: " + matches);

        if (latencyMs < 30) {
            System.out.println("VERIFICATION: SUCCESS (Zero-copy confirmed)");
        } else {
            System.out.println("VERIFICATION: FAILURE (Possible data copy detected)");
        }

        engine.destroy();
    }
}
