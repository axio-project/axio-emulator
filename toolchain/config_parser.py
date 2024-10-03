
class Config:
    # Tunable parameters
    kAppCoreNum = 0
    kDispQueueNum = 0
    kAppTxMsgBatchSize = 0
    kAppRxMsgBatchSize = 0
    kDispTxBatchSize = 0
    kDispRxBatchSize = 0
    kNICTxPostSize = 0
    kNICRxPostSize = 0
    # Datapath configs
    workloads_map_pipephases = {}      # workload id -> pipe_phases (str)
    workloads_map_remote_cores = {}    # workload id -> remote cores (list)
    workloads_map_app_cores = {}       # workload id -> app cores (lists)
    workloads_map_disp_cores = {}      # workload id -> disp cores (lists)
    # Server configs
    numa = ''
    phy_port = ''
    iteration = ''
    duration = ''
    # Addresses configs
    local_ip = ''
    remote_ip = ''
    local_mac = ''
    remote_mac = ''
    device_pcie = ''

    # Init from config file, the file also will be used for tuning
    def __init__(self, config_file_path):
        self.config_file_path = config_file_path
        self.load_config()
        # generate the output directory
        self.output_config_file_path = config_file_path + ".out"


    def load_config(self):
        with open(self.config_file_path, "r") as f:
            for line in f:
                # skip the line if it is a comment or empty line
                if line.startswith("#") or line.strip() == "":
                    continue
                # exit if the key is not in the Config class
                if ":" not in line:
                    raise ValueError(f"Invalid configuration: {line}")
                if line.count(":") == 1:
                    key, value = line.strip().split(":")
                    # remove the leading and trailing whitespaces
                    key = key.strip()
                    value = value.strip()
                    if hasattr(Config, key):
                        if key.startswith("k"):
                            setattr(Config, key, int(value))
                        else:
                            setattr(Config, key, value)
                else:
                    values = line.strip().split(":")
                    # remove the leading and trailing whitespaces
                    values = [value.strip() for value in values]
                    # the first value should be "workload"
                    if values[0] != "workload":
                        raise ValueError(f"Invalid configuration: {line}")
                    # workload id -> pipe_phases (str)
                    self.workloads_map_pipephases[int(values[1])] = values[2]
                    # workload id -> remote cores (list)
                    self.workloads_map_remote_cores[int(values[1])] = []
                    for core in values[3].split(","):
                        self.workloads_map_remote_cores[int(values[1])].append(int(core))
                    # workload id -> app cores (lists)
                    self.workloads_map_app_cores[int(values[1])] = []
                    groups = values[4].split("|")
                    for group in groups:
                        tmp = []
                        if '-' in group:
                            start, end = group.split("-")
                            for core in range(int(start), int(end) + 1):
                                tmp.append(core)
                            self.workloads_map_app_cores[int(values[1])].append(tmp)
                        else:
                            for core in group.split(","):
                                tmp.append(int(core))
                            self.workloads_map_app_cores[int(values[1])].append(tmp)
                    # workload id -> disp cores (lists)
                    self.workloads_map_disp_cores[int(values[1])] = []
                    groups = values[5].split("|")
                    for group in groups:
                        tmp = []
                        for core in group.split(","):
                            tmp.append(int(core))
                        self.workloads_map_disp_cores[int(values[1])].append(tmp)

    def print_tunable_paras(self):
        print("\033[1;33m" + "==========Current Tunable Parameter Values:==========" + "\033[0m")
        for key in dir(Config):
            if key.startswith("k"):
                print(f"{key}: {getattr(Config, key)}")

    def verify_tunable_paras(self):
        # Check the datapath configs
        app_group_num = []
        core_num = 0
        for workload_id, app_cores in self.workloads_map_app_cores.items():
            app_group_num.append(len(app_cores))
            for group in app_cores:
                core_num += len(group)
        if core_num != Config.kAppCoreNum:
            raise ValueError(f"Invalid configuration: kAppCoreNum should be {core_num}")
        core_num = 0
        idx = 0
        for workload_id, disp_cores in self.workloads_map_disp_cores.items():
            if len(disp_cores) != app_group_num[idx]:
                raise ValueError(f"Invalid configuration: disp group should be equal to app group")
            for group in disp_cores:
                if len(group) != 1:
                    raise ValueError(f"Invalid configuration: each disp group should have only one core")
                core_num += len(group)
            idx += 1
        if core_num != Config.kDispQueueNum:
            raise ValueError(f"Invalid configuration: kDispQueueNum should be {core_num}")
        print("\033[1;33m" + "==========Tunable Parameter Verification Passed==========" + "\033[0m")

    def write_back(self):
        with open(self.output_config_file_path, "w") as f:
            f.write("# -----------------PipeTune Tuner Configuration-----------------\n")
            for key in dir(Config):
                if key.startswith("k"):
                    f.write(f"{key} : {getattr(Config, key)}\n")
            # Generate PipeTune datapath config
            f.write(f"\n")
            f.write(f"# -----------------PipeTune Datapath Configuration-----------------\n")
            ## Generate the pipe phases
            for workload_id, pipe_phases in self.workloads_map_pipephases.items():
                f.write(f"workload : {workload_id} : {pipe_phases} : ")
                ## Generate the remote cores
                f.write(f"{','.join([str(core) for core in self.workloads_map_remote_cores[workload_id]])} : ")
                ## Generate the app cores
                f.write(f"{'|'.join([','.join([str(core) for core in group]) for group in self.workloads_map_app_cores[workload_id]])} : ")
                ## Generate the disp cores
                f.write(f"{'|'.join([','.join([str(core) for core in group]) for group in self.workloads_map_disp_cores[workload_id]])}\n")
            # Generate PipeTune server config
            f.write(f"\n")
            f.write(f"numa : {self.numa}\n")
            f.write(f"phy_port : {self.phy_port}\n")
            f.write(f"iteration : {self.iteration}\n")
            f.write(f"duration : {self.duration}\n")
            # Generate PipeTune addresses config
            f.write(f"\n")
            f.write(f"local_ip : {self.local_ip}\n")
            f.write(f"remote_ip : {self.remote_ip}\n")
            f.write(f"local_mac : {self.local_mac}\n")
            f.write(f"remote_mac : {self.remote_mac}\n")
            f.write(f"device_pcie : {self.device_pcie}\n")


