import argparse, os
import subprocess

from config_parser import Config

if __name__ == "__main__":
    # Parse command line arguments
    parser = argparse.ArgumentParser(description="Toolchain for Axio, including init, verify, print, and tune")
    parser.add_argument("-i", "--init", action="store_true", default=None, help="Initialize Axio environment")
    parser.add_argument("-c", "--config", type=str, help="Path to the configuration file")
    parser.add_argument("-v", "--verify", action="store_true", default=None, help="Verify the configuration file")
    parser.add_argument("-p", "--print", action="store_true", default=None, help="Print the tunable parameters")
    args = parser.parse_args()

    cur_path = os.path.dirname(os.path.abspath(__file__))
    root_path = os.path.dirname(cur_path + "/../")

    # Initialize Axio environment
    if args.init is not None:
        init_script_path = root_path + "/scripts/init.sh"
        run_command = f"sudo bash {init_script_path}"
        subprocess.run(run_command, shell=True)
        exit(0)

    # Load configuration
    if args.config is None:
        print("[ERROR] Please specify the configuration file via -c")
        exit(1)
    config = Config(args.config)
    print_flag = False
    verify_flag = False

    if args.print is not None:
        config.print_tunable_paras()
        print_flag = True
    
    # Verify configuration
    if args.verify is not None:
        config.verify_tunable_paras()
        verify_flag = True
