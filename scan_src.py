import sys
import glob

# add all local source files
sources = glob.glob("./src/**/*.c", recursive=True) + glob.glob("./src/**/*.cpp", recursive=True) + glob.glob("./src/**/*.cc", recursive=True)
    
for i in sources:
    print(i)
    