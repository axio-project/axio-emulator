import sys

INPUT_FILE = sys.argv[1]

cpu_util = {}
num_samples = {}

with open(INPUT_FILE) as f1:
    for line in f1:
        elements = line.split()
        if len(elements) == 10 and elements[3].isdigit():
            cpu = int(elements[3])
            util = float(elements[9])
            if cpu not in cpu_util:
                cpu_util[cpu] = (100 - util)
                num_samples[cpu] = 1
            else:
                cpu_util[cpu] += (100 - util)
                num_samples[cpu] += 1
        elif len(elements) == 8 and elements[1].isdigit():
            cpu = int(elements[1])
            util = float(elements[7])
            if cpu not in cpu_util:
                cpu_util[cpu] = (100 - util)
                num_samples[cpu] = 1
            else:
                cpu_util[cpu] += (100 - util)
                num_samples[cpu] += 1

total_util = 0
num_cpus = 0
for cpu in cpu_util:
    if num_samples[cpu] != 0:
        cpu_util[cpu] /= num_samples[cpu]
        total_util += cpu_util[cpu]
        num_cpus += 1

for cpu in cpu_util:
    if num_samples[cpu] != 0:
        print("core",cpu,": {:.2f}%".format(cpu_util[cpu]))
# print("num_samples: ",num_samples)
print("avg_cpu_util: {:.2f}%".format(total_util/num_cpus))
