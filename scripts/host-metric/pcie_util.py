import sys

INPUT_FILE = sys.argv[1]

line_idx = 0
io_read_idx = 0
io_write_idx = 0
with open(INPUT_FILE) as f1:
    # find first line which contains 'PCIRdCur' and 'ItoM'
    for line in f1:
        elements = line.split(',')
        if 'PCIRdCur' in elements and 'ItoM' in elements:
            for i in range(len(elements)):
                if elements[i] == 'PCIRdCur':
                    io_read_idx = i
                if elements[i] == 'ItoM':
                    io_write_idx = i
            break
        line_idx += 1
    # record the io read and write data
    io_read_data = {}   # socket id : [read_cnt, read_miss, read_hit]
    io_write_data = {}  # socket id : [write_cnt, write_miss, write_hit]
    temp_idx = 0
    lines = f1.readlines()
    for line in lines[line_idx:]:
        elements = line.split(',')
        if 'PCIRdCur' in elements and 'ItoM' in elements:
            break
        socket_id = int(elements[0])
        if temp_idx == 0:
            # record read_cnt and write_cnt
            io_read_data[socket_id] = [int(elements[io_read_idx]), 0, 0]
            io_write_data[socket_id] = [int(elements[io_write_idx]), 0, 0]
            temp_idx += 1
        elif temp_idx == 1:
            # record read_miss and write_miss
            io_read_data[socket_id][1] = int(elements[io_read_idx])
            io_write_data[socket_id][1] = int(elements[io_write_idx])
            temp_idx += 1
        elif temp_idx == 2:
            # record read_hit and write_hit
            io_read_data[socket_id][2] = int(elements[io_read_idx])
            io_write_data[socket_id][2] = int(elements[io_write_idx])
            temp_idx = 0

# print the io read and write data
for socket_id in io_read_data:
    print("------Socket " + str(socket_id) + "------")
    print("Read_cnt: " + str(io_read_data[socket_id][0]))
    print("Read_miss: " + str(io_read_data[socket_id][1]))
    print("Read_hit: " + str(io_read_data[socket_id][2]))
    print("Write_cnt: " + str(io_write_data[socket_id][0]))
    print("Write_miss: " + str(io_write_data[socket_id][1]))
    print("Write_hit: " + str(io_write_data[socket_id][2]))
    print("Read Miss Rate: {:.2f}".format(io_read_data[socket_id][1] / io_read_data[socket_id][0]))
    print("Write Miss Rate: {:.2f}".format(io_write_data[socket_id][1] / io_write_data[socket_id][0]))

    # print("socket " + str(socket_id) + ",read_cnt:" + str(io_read_data[socket_id][0]) + ",read_miss:" + str(io_read_data[socket_id][1]) + ",read_hit:" + str(io_read_data[socket_id][2]))
    # print("socket " + str(socket_id) + ",write_cnt:" + str(io_write_data[socket_id][0]) + ",write_miss:" + str(io_write_data[socket_id][1]) + ",write_hit:" + str(io_write_data[socket_id][2]))




