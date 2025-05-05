from config_parser import Config
import subprocess, time

class Tuner:
    def __init__(self, config, print_flag=False, verify_flag=False, iter_num=0, root_path=""):
        self.config = config
        self.print_flag = print_flag
        self.verify_flag = verify_flag
        self.config.write_back()    # Copy the user configuration as default 
        self.root_path = root_path
        subprocess.run(f"mkdir -p {root_path}/tmp", shell=True)

        # Tuner parameters
        self.iter_num = iter_num
        self.cur_iter = 0
        self.sample_iter = int(config.iteration) / 2  # use the middle iteration of Axio datapath as the sample point

        # Axio datapath parameters
        self.stage_names = ["app_tx", "app_rx", "disp_tx", "disp_rx", "nic_tx", "nic_rx"]
        self.compl_time = [0, 0, 0, 0, 0, 0]    # average completion time of app tx, app rx, disp tx, disp rx, nic tx, nic rx
        self.remote_compl_time = [0, 0, 0, 0, 0, 0]
        self.stall_time = [0, 0, 0, 0]          # average stall time of app tx, app rx, disp tx, disp rx
        self.remote_stall_time = [0, 0, 0, 0]
        self.e2e_throughput = 0
        self.remote_e2e_throughput = 0
        self.flag_remote_init = False

    def init_remote(self):
        if self.root_path == "":
            raise ValueError("Invalid root path")
        # call remote tuner to init the default configuration
        print("\033[1;33m" + "[INFO]"  + "\033[0m" + " Try to init remote tuner......")
        subprocess.run(f"bash {self.root_path}/scripts/ssh_command.sh \" mkdir -p {self.root_path}/tmp \"", shell=True)
        subprocess.run(f"bash {self.root_path}/scripts/ssh_command.sh \"python3 {self.root_path}/toolchain/main.py -c {self.root_path}/config/send_config -t 0\"", shell=True)
        subprocess.run(f"bash {self.root_path}/scripts/scp_command.sh --remote-to-local \"{self.root_path}/config/send_config\" \"{self.root_path}/config/remote_send_config\"", shell=True)
        self.remote_config = Config(f"{self.root_path}/config/remote_send_config")
        self.remote_config.write_back()
        subprocess.run(f"bash {self.root_path}/scripts/scp_command.sh --local-to-remote \"{self.root_path}/config/remote_send_config\" \"{self.root_path}/config/send_config.out\"", shell=True)
        print("\033[1;33m" + "[INFO]" + "\033[0m" + " Remote tuner init done.")
        self.flag_remote_init = True

    def run(self):
        for i in range(self.iter_num):
            self.cur_iter = i
            print("\033[1;33m" + "==========Current iteration: " + str(self.cur_iter) + "==========" + "\033[0m")
            # ==========Step 1: execute the Axio datapath for once==========
            ### create output file
            output_file = self.root_path + "/tmp/axio_iter_" + str(self.cur_iter) + ".log"
            remote_output_file = self.root_path + "/tmp/remote_axio_iter_" + str(self.cur_iter) + ".log"
            with open(output_file, "w") as f:
                f.write("")   # clear the file
            ### start Axio datapath
            run_command = f"sudo {self.root_path}/build/axio > {output_file}"
            process = subprocess.Popen(run_command, shell=True)
            subprocess.run(f"bash {self.root_path}/scripts/ssh_command.sh \"touch {output_file}\"", shell=True)
            remote_process = subprocess.Popen(f"bash {self.root_path}/scripts/ssh_command.sh \"cd {self.root_path} ; sudo ./build/axio > {output_file}\"", shell=True)
            ### record host metrics
            run_command = f"sudo bash {self.root_path}/scripts/host-metric/record-host-metrics.sh"
            #### wait until the Axio datapath has run for a while
            wait_seconds = float(self.config.iteration) * float(self.config.duration) / 2
            time.sleep(wait_seconds)
            metric_process = subprocess.Popen(run_command, shell=True)
            subprocess.run(f"bash {self.root_path}/scripts/ssh_command.sh \"cd {self.root_path} ; sudo bash ./scripts/host-metric/record-host-metrics.sh\"", shell=True)
            #### wait until the Axio datapath has finished
            process.wait()
            remote_process.wait()
            metric_process.wait()
            # ==========Step 2: read and parse the Axio datapath output==========
            self.e2e_throughput = self.parse_output(output_file, self.compl_time, self.stall_time, self.sample_iter)
            ### scp the output file from remote to local
            subprocess.run(f"bash {self.root_path}/scripts/scp_command.sh --remote-to-local \"{output_file}\" \"{remote_output_file}\"", shell=True)
            self.remote_e2e_throughput = self.parse_output(remote_output_file, self.remote_compl_time, self.remote_stall_time, self.sample_iter)
            # ==========Step 3: diagnose and tune==========
            print("\033[1;33m" + "[INFO]"  + "\033[0m" + " Diagnosing the contention point for local......")
            metric_file_dir = self.root_path + "/scripts/host-metric/reports"
            metric_file_path = metric_file_dir + "/report.rpt"
            remote_metric_file_path = metric_file_dir + "/remote_report.rpt"
            self.diagnose(self.compl_time, self.stall_time, metric_file_path)
            print("\033[1;33m" + "[INFO]"  + "\033[0m" + " Diagnosing the contention point for remote......")
            subprocess.run(f"bash {self.root_path}/scripts/scp_command.sh --remote-to-local \"{metric_file_path}\" \"{remote_metric_file_path}\"", shell=True)
            self.diagnose(self.remote_compl_time, self.remote_stall_time, remote_metric_file_path)
            # ==========Step 4: update the Axio datapath configuration==========
            self.config.write_back()
            if self.verify_flag:
                self.config.verify_tunable_paras()

    def diagnose(self, compl_time, stall_time, metrics_file):
        # read the metrics file
        numa_node = int(self.config.numa)
        io_read_miss = 0
        io_write_miss = 0
        llc_read_miss = 0
        llc_write_miss = 0
        with open(metrics_file, "r") as f:
            lines = f.readlines()
            IO_flag = False
            for line_idx in range(len(lines)):
                line = lines[line_idx]
                if "Socket " + str(numa_node) in line:
                    IO_flag = True
                elif "IO Read Miss Rate" in line and IO_flag:
                    io_read_miss = float(line.split(":")[1])
                elif "IO Write Miss Rate" in line and IO_flag:
                    io_write_miss = float(line.split(":")[1])
                elif "LLC-load-misses-rate" in line:
                    llc_read_miss = float(line.split(":")[1])
                elif "LLC-store-misses-rate" in line:
                    llc_write_miss = float(line.split(":")[1])
                line_idx += 1
        if self.print_flag:
            print("\033[1;33m" + "==========Performance Statistics==========" + "\033[0m")
            print("\033[1;34m" + "[DEBUG]" + "\033[0m" + " End-to-end Throughput: " + str(self.e2e_throughput))
            print("\033[1;34m" + "[DEBUG]" + "\033[0m" + " Completion time: " + str(compl_time))
            print("\033[1;34m" + "[DEBUG]" + "\033[0m" + " Stall time: " + str(stall_time))
            print("\033[1;34m" + "[DEBUG]" + "\033[0m" + " IO Read Miss Rate: " + str(io_read_miss))
            print("\033[1;34m" + "[DEBUG]" + "\033[0m" + " IO Write Miss Rate: " + str(io_write_miss))
            print("\033[1;34m" + "[DEBUG]" + "\033[0m" + " LLC Read Miss Rate: " + str(llc_read_miss))
            print("\033[1;34m" + "[DEBUG]" + "\033[0m" + " LLC Write Miss Rate: " + str(llc_write_miss))
        # search the most critical pipe phase
        max_compl_time = max(compl_time)
        max_compl_idx = compl_time.index(max_compl_time)
        print("\033[1;33m" + "[INFO]"  + "\033[0m" + " The most critical pipe phase is: " + self.stage_names[max_compl_idx])
        # compare the completion time and stall time
        if max_compl_time - stall_time[max_compl_idx] < stall_time[max_compl_idx]:
            print("\033[1;33m" + "[INFO]"  + "\033[0m" + " The completion time < stall time. Contention point is C1.")
            return [self.stage_names[max_compl_idx], "C1"]
        else:
            print("\033[1;33m" + "[INFO]"  + "\033[0m" + " The completion time > stall time. Contention point is C2 or C4.")
            return [self.stage_names[max_compl_idx], "C2 or C4"]




    def parse_output(self, output_file, compl_time, stall_time, sample_iter):
        e2e_throughput = 0
        with open(output_file, "r") as f:
            lines = f.readlines()
            cur_iter = 0
            line_idx = 0
            flag_sample = False
            for line in lines:
                if "Perf Statistics" in line:
                    cur_iter += 1
                    if cur_iter == sample_iter:
                        flag_sample = True
                        break
                line_idx += 1
            if flag_sample:
                for line in lines[line_idx:line_idx+9]:
                    if "End-to-end" in line:
                        e2e_throughput = float(line.split()[1])
                    if "app_tx" in line:
                        # split the line by some spaces
                        values = line.split()
                        compl_time[0] = float(values[2])
                        stall_time[0] = float(values[3])
                    elif "app_rx" in line:
                        values = line.split()
                        compl_time[1] = float(values[2])
                        stall_time[1] = float(values[3])
                    elif "disp_tx" in line:
                        values = line.split()
                        compl_time[2] = float(values[2])
                        stall_time[2] = float(values[3])
                    elif "disp_rx" in line:
                        values = line.split()
                        compl_time[3] = float(values[2])
                        stall_time[3] = float(values[3])
                    elif "nic_tx" in line:
                        values = line.split()
                        compl_time[4] = float(values[2])
                    # \todo: currently, the nic_rx completion time is not precise, need to be fixed
                    # elif "nic_rx" in line:
                    #     values = line.split()
                    #     self.compl_time[5] = float(values[2])
                
            else:
                raise ValueError("Invalid Axio output file, cannot find the sample iteration at " + str(output_file))
        return e2e_throughput





