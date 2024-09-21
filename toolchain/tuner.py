from config_parser import Config
import subprocess

class Tuner:
    def __init__(self, config, print_flag=False, verify_flag=False, iter_num=0, root_path=""):
        self.config = config
        self.print_flag = print_flag
        self.verify_flag = verify_flag
        self.config.write_back()    # Copy the user configuration as default 
        self.root_path = root_path

        # Tuner parameters
        self.iter_num = iter_num
        self.cur_iter = 0
        self.sample_iter = int(config.iteration) / 2  # use the middle iteration of PipeTune datapath as the sample point

        # PipeTune datapath parameters
        self.compl_time = [0, 0, 0, 0, 0, 0]    # average completion time of app tx, app rx, disp tx, disp rx, nic tx, nic rx
        self.stall_time = [0, 0, 0, 0]          # average stall time of app tx, app rx, disp tx, disp rx

    def run(self):
        for i in range(self.iter_num):
            self.cur_iter = i
            print("==========Current iteration: " + str(self.cur_iter) + "==========")
            # Step 1: execute the PipeTune datapath for once
            # create output file
            with open(self.root_path + "/tmp/pipetune_iter_" + str(self.cur_iter) + ".log", "w") as f:
                f.write("")   # clear the file
            run_command = f"sudo {self.root_path}/build/pipetune > {self.root_path}/tmp/pipetune_iter_" + str(self.cur_iter) + ".log"
            subprocess.run(run_command, shell=True)
            # Step 2: read and parse the PipeTune datapath output
            self.parse_output()
            # Step 3: update the PipeTune datapath configuration
            # self.config.write_back()

            if self.print_flag:
                self.config.print_tunable_paras()
            if self.verify_flag:
                self.config.verify_tunable_paras()

    def parse_output(self):
        with open("./tmp/pipetune_iter_" + str(self.cur_iter) + ".log", "r") as f:
            lines = f.readlines()
            cur_iter = 0
            line_idx = 0
            flag_sample = False
            for line in lines:
                if "Perf Statistics" in line:
                    cur_iter += 1
                    if cur_iter == self.sample_iter:
                        flag_sample = True
                        break
                line_idx += 1
            if flag_sample:
                for line in lines[line_idx:line_idx+9]:
                    if "app_tx" in line:
                        # split the line by some spaces
                        values = line.split()
                        self.compl_time[0] = float(values[2])
                        self.stall_time[0] = float(values[3])
                    elif "app_rx" in line:
                        values = line.split()
                        self.compl_time[1] = float(values[2])
                        self.stall_time[1] = float(values[3])
                    elif "disp_tx" in line:
                        values = line.split()
                        self.compl_time[2] = float(values[2])
                        self.stall_time[2] = float(values[3])
                    elif "disp_rx" in line:
                        values = line.split()
                        self.compl_time[3] = float(values[2])
                        self.stall_time[3] = float(values[3])
                    elif "nic_tx" in line:
                        values = line.split()
                        self.compl_time[4] = float(values[2])
                    # \todo: currently, the nic_rx completion time is not precise, need to be fixed
                    # elif "nic_rx" in line:
                    #     values = line.split()
                    #     self.compl_time[5] = float(values[2])
            else:
                raise ValueError("Invalid PipeTune output file, cannot find the sample iteration (" + str(self.sample_iter) + ")")





