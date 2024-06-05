import os
import subprocess
import time

pid = os.getpid()
# os.sched_setaffinity(pid, {21,22,23,24})

# Path to the cgroup v2 hierarchy
cgroup_path = "/cgroup2"

# Name of the new cgroup
cgroup_name = "memcached"

# Path to the new cgroup
new_cgroup_path = os.path.join(cgroup_path, cgroup_name)

# Create the new cgroup
os.makedirs(new_cgroup_path, exist_ok=True)

# Set the memory limit (in bytes)
with open(os.path.join(new_cgroup_path, "memory.high"), "w") as f:
    f.write("12G")  

# Run the command as a subprocess
#command = ["python", "/users/YuqiLi/my_higgs.py"]
#command = ["/users/YuqiLi/cfm/quicksort/quicksort", "16384"]
#command = ["/users/YuqiLi/XSBench/openmp-threading/", "-t", "4", "-g", "72000", "-p", "1000000"]
#command = ["taskset","-c", "4,6,8,10,12,14,16,18","/users/YuqiLi/gapbs/pr", "-g", "27"]
#command = ["taskset","-c", "11,12,13,14,15,16", "python", "/mydata/fastswap/higgs.py"]
command = ["taskset", "-c", "0,1,2,3,4,5,6,7", "memcached", "-u", "YuqiLi", "-m", "32768", "-p", "112133", "-t", "8"]

#env = dict(os.environ)  
#env["OMP_NUM_THREADS"] = "8"

start_time = time.time()
process = subprocess.Popen(command, stderr=subprocess.PIPE)

# Add the process to the cgroup
with open(os.path.join(new_cgroup_path, "cgroup.procs"), "w") as f:
    f.write(str(process.pid))

# Wait for the process to finish
process.wait()
end_time = time.time()

if process.returncode == 0:
    print("workload finish successfully.\n")
else:
    stderr = process.stderr.read()
    print("workload exit with error code: {}".format(process.returncode))
    print(stderr)



# Print the runtime of the command
print(f"Runtime: {end_time - start_time} seconds")

# Remove the cgroup after the command has finished
# os.rmdir(new_cgroup_path)