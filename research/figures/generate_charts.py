import matplotlib.pyplot as plt
import numpy as np

# Set publication style
plt.style.use('seaborn-v0_8-paper')
plt.rcParams.update({'font.size': 12, 'axes.labelsize': 14, 'axes.titlesize': 16})

def generate_throughput_chart():
    batch_sizes = [10, 64, 512, 4096, 32768, 262144, 2097152, 16777216]
    scalar_tput = [2.1, 2.1, 2.1, 2.1, 2.1, 2.1, 2.1, 2.1]
    bitsliced_tput = [0.1, 3.8, 3.82, 3.85, 3.87, 3.88, 3.88, 3.88]
    gpu_tput = [0, 0, 0, 0, 0.5, 2.1, 8.5, 10.2]

    plt.figure(figsize=(10, 6))
    plt.plot(batch_sizes, scalar_tput, label='Scalar (Baseline)', marker='o', linestyle='--')
    plt.plot(batch_sizes, bitsliced_tput, label='Bit-Sliced JIT (CPU)', marker='s', linewidth=2)
    plt.plot(batch_sizes, gpu_tput, label='Metal GPGPU (Apple Silicon)', marker='^', linewidth=2)

    plt.xscale('log')
    plt.xlabel('Batch Size (Rows)')
    plt.ylabel('Throughput (Billion Rows/sec)')
    plt.title('AarchGate Throughput Scaling')
    plt.grid(True, which="both", ls="-", alpha=0.2)
    plt.legend()
    plt.savefig('research/figures/throughput_scaling.png', dpi=300)
    plt.close()

def generate_energy_chart():
    labels = ['XGBoost (Native)', 'AarchGate-ML']
    energy = [21428, 195] # nanoJoules per row

    plt.figure(figsize=(8, 6))
    bars = plt.bar(labels, energy, color=['gray', 'blue'])
    plt.ylabel('Energy Consumption (nanoJoules / row)')
    plt.title('Energy Efficiency Comparison')
    plt.yscale('log')
    
    for bar in bars:
        yval = bar.get_height()
        plt.text(bar.get_x() + bar.get_width()/2, yval, f'{yval} nJ', va='bottom', ha='center')

    plt.savefig('research/figures/energy_efficiency.png', dpi=300)
    plt.close()

if __name__ == "__main__":
    generate_throughput_chart()
    generate_energy_chart()
    print("Charts generated successfully.")
