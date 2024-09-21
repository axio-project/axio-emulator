from config_parser import Config
import subprocess

def parse_output(output_file, compl_time, stall_time, sample_iter):
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
            raise ValueError("Invalid PipeTune output file, cannot find the sample iteration at " + str(output_file))

class Tuner:
    def __init__(self, config, print_flag=False, verify_flag=False, iter_num=0, root_path=""):
        self.config = config
        self.print_flag = print_flag
        self.verify_flag = verify_flag
        self.config.write_back()    # Copy the user configuration as default 
        self.root_path = root_path
        subprocess.run(f"mkdir -p {root_path}/tmp", shell=True)
        # call remote tuner to init the default configuration
        print("[INFO] Try to init remote tuner......")
        subprocess.run(f"bash {root_path}/scripts/ssh_command.sh \" mkdir -p {root_path}/tmp \"", shell=True)
        subprocess.run(f"bash {root_path}/scripts/ssh_command.sh \"python3 {root_path}/toolchain/main.py -c {root_path}/config/send_config -t 0\"", shell=True)
        subprocess.run(f"bash {root_path}/scripts/scp_command.sh --remote-to-local \"{root_path}/config/send_config\" \"{root_path}/config/remote_send_config\"", shell=True)
        self.remote_config = Config(f"{root_path}/config/remote_send_config")
        self.remote_config.write_back()
        print("[INFO] Remote tuner init done.")

        # Tuner parameters
        self.iter_num = iter_num
        self.cur_iter = 0
        self.sample_iter = int(config.iteration) / 2  # use the middle iteration of PipeTune datapath as the sample point

        # PipeTune datapath parameters
        self.compl_time = [0, 0, 0, 0, 0, 0]    # average completion time of app tx, app rx, disp tx, disp rx, nic tx, nic rx
        self.remote_compl_time = [0, 0, 0, 0, 0, 0]
        self.stall_time = [0, 0, 0, 0]          # average stall time of app tx, app rx, disp tx, disp rx
        self.remote_stall_time = [0, 0, 0, 0]

    def run(self):
        for i in range(self.iter_num):
            self.cur_iter = i
            print("==========Current iteration: " + str(self.cur_iter) + "==========")
            # ==========Step 1: execute the PipeTune datapath for once==========
            ### create output file
            output_file = self.root_path + "/tmp/pipetune_iter_" + str(self.cur_iter) + ".log"
            remote_output_file = self.root_path + "/tmp/remote_pipetune_iter_" + str(self.cur_iter) + ".log"
            with open(output_file, "w") as f:
                f.write("")   # clear the file
            run_command = f"sudo {self.root_path}/build/pipetune > {output_file}"
            process = subprocess.Popen(run_command, shell=True)
            ### start remote PipeTune datapath
            subprocess.run(f"bash {self.root_path}/scripts/ssh_command.sh \"touch {output_file}\"", shell=True)
            subprocess.run(f"bash {self.root_path}/scripts/ssh_command.sh \"cd {self.root_path} ; sudo ./build/pipetune > {output_file}\"", shell=True)
            process.wait()

            # ==========Step 2: read and parse the PipeTune datapath output==========
            parse_output(output_file, self.compl_time, self.stall_time, self.sample_iter)
            ### scp the output file from remote to local
            subprocess.run(f"bash {self.root_path}/scripts/scp_command.sh --remote-to-local \"{output_file}\" \"{remote_output_file}\"", shell=True)
            parse_output(remote_output_file, self.remote_compl_time, self.remote_stall_time, self.sample_iter)
            if self.print_flag:
                print("DEBUG:")
                print(f"Completion and stall time at iteration {str(self.iter_num)}:")
                print("Local completion time: " + str(self.compl_time))
                print("Local stall time: " + str(self.stall_time))
                print("Remote completion time: " + str(self.remote_compl_time))
                print("Remote stall time: " + str(self.remote_stall_time))

            # ==========Step 3: update the PipeTune datapath configuration==========
            # self.config.write_back()
            if self.print_flag:
                self.config.print_tunable_paras()
            if self.verify_flag:
                self.config.verify_tunable_paras()






