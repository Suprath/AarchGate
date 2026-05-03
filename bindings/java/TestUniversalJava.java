import com.aarchgate.engine.AarchGateEngine;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class TestUniversalJava {
    public static void main(String[] args) {
        System.out.println("=== Universal Java SDK Verification Test ===");
        
        AarchGateEngine engine = new AarchGateEngine();

        AarchGateEngine.FieldDescriptor[] fields = new AarchGateEngine.FieldDescriptor[] {
            new AarchGateEngine.FieldDescriptor("Field0", 0, 64, 3), // UINT64
            new AarchGateEngine.FieldDescriptor("Field1", 8, 64, 3),
            new AarchGateEngine.FieldDescriptor("Field2", 16, 64, 3)
        };
        
        engine.registerSchema("GenericSchema", fields, 64);
        
        long logicPtr = AarchGateEngine.createUniversalTestLogic();
        engine.setLogic("GenericSchema", logicPtr, 1); // 1 = SCALAR
        
        int numRows = 1000000;
        int stride = 64;
        int capacity = numRows * stride;
        
        // Use direct buffer for zero-copy
        ByteBuffer data = ByteBuffer.allocateDirect(capacity);
        data.order(ByteOrder.nativeOrder());
        
        for (int i = 0; i < numRows; i++) {
            data.putLong(i * stride + 0, i);
            data.putLong(i * stride + 8, 1000);
            
            long field2 = (i % 2 == 0) ? (i + 500) : (i + 2000);
            data.putLong(i * stride + 16, field2);
        }
        
        long startTime = System.nanoTime();
        long matches = engine.execute(data, numRows);
        long endTime = System.nanoTime();
        
        double timeMs = (endTime - startTime) / 1000000.0;
        
        System.out.println("Matches: " + matches + " (Expected: 500000)");
        System.out.printf("Time: %.2f ms\n", timeMs);
        
        if (matches == 500000) {
            System.out.println("[PASS] Universal Java SDK Test");
            engine.destroy();
            System.exit(0);
        } else {
            System.out.println("[FAIL] Incorrect matches");
            engine.destroy();
            System.exit(1);
        }
    }
}
