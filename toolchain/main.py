import argparse, os
import subprocess

from config_parser import Config
from tuner import Tuner

if __name__ == "__main__":
    # Parse command line arguments
    parser = argparse.ArgumentParser(description="Toolchain for PipeTune, including init, verify, print, and tune")
    parser.add_argument("-i", "--init", action="store_true", default=None, help="Initialize PipeTune environment")
    parser.add_argument("-c", "--config", type=str, help="Path to the configuration file")
    parser.add_argument("-v", "--verify", action="store_true", default=None, help="Verify the configuration file")
    parser.add_argument("-p", "--print", action="store_true", default=None, help="Print the tunable parameters")
    parser.add_argument("-t", "--tune", type=int, default=None, help="Run the tuner, specify the number of iterations")
    args = parser.parse_args()

    cur_path = os.path.dirname(os.path.abspath(__file__))
    root_path = os.path.dirname(cur_path + "/../")

    # Initialize PipeTune environment
    if args.init is not None:
        init_script_path = root_path + "/scripts/init.sh"
        run_command = f"sudo bash {init_script_path}"
        subprocess.run(run_command, shell=True)
        exit(0)

    # Load configuration
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

    if args.tune is not None:
        # Initialize the tuner
        tuner = Tuner(config, print_flag, verify_flag, args.tune, root_path)
        tuner.run()

    # Initialize the tuner
    # tuner = Tuner(config)

    # Run the tuner
    # tuner.run()