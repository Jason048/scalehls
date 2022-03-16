#this assumes that input C file uses output from scalehls

import os
import shutil
import pandas as pd
import argparse
import copy
import treelib

#AutoHLS dependencies
from lib import RandInit as RT
from lib import DSEinputparse as INPAR
from lib import pyScaleHLS as PYSHLS
from lib import DSE_main as DMain
from lib import dsepattern as DPAT

def print_optknobs(opt_knobs, opt_knob_names):
    for i in range(len(opt_knobs) - 1):
        print("{0}: {1}".format(opt_knob_names[i+1], opt_knobs[i+1]))

def print_variables(var_forlist, var_arraylist_sized):
    print("Loops")
    for item in var_forlist :
        print(item)
    
    print("Arrays")
    for item in var_arraylist_sized :
        print(item)    

def sortbyhotness(inputarray):  #insertion sort
    sortedarray = copy.deepcopy(inputarray)
    try:
        while True:
            sortedarray.remove("")
    except ValueError:
        pass
    
    for i in range(1, len(sortedarray)): 
        key = sortedarray[i]
        j = i-1
        while j >= 0 and key[-1] > sortedarray[j][-1] :
                sortedarray[j + 1] = sortedarray[j]
                j -= 1
        sortedarray[j + 1] = key
    return sortedarray

def main():
    #temp files store location
    proj_name = ""
    tar_dir = "generated_files"

    parser = argparse.ArgumentParser(prog='Cfor_lex')
    parser.add_argument('-if', dest='inputfile',
                    metavar="inputfile",
                    help='maunual userinput file')
    parser.add_argument('-i', dest='input',
                        metavar="input",
                        help='maunual input file')
    parser.add_argument('-t', dest='topfun',
                    metavar="topfun",
                    help='top_function')
    parser.add_argument('-p', dest='part',
                    metavar="part",
                    help='SoC part')
    parser.add_argument('-tag', dest='tracker',
                    metavar="tracker",
                    help='for multiple instances')
    parser.add_argument('-c', '--clean_temptemp', action='store_true',
                help='clean temptemp files')
    parser.add_argument('-cf', '--clean_temp', action='store_true',
                help='clean all temp files')
    parser.add_argument('-cff', '--clean_all', action='store_true',
                help='clean all generated files')               
    opts = parser.parse_args()

    if opts.tracker:
        tar_dir = tar_dir + "_" + opts.tracker
        proj_name = opts.tracker

    if opts.clean_temptemp:
        try:
            shutil.rmtree(tar_dir + "/scalehls_dse_temp")
        except OSError as e:
            print("Did not find \"scalehls_dse_temp\"")
            # print("Error: %s : %s" % (os.file_path, e.strerror))
        try:
            shutil.rmtree(tar_dir + "/vhls_dse_temp")
        except OSError as e:
            print("Did not find \"vhls_dse_temp\"")
            # print("Error: %s : %s" % (os.file_path, e.strerror))
        return 0

    if opts.clean_temp:
        #remove generated files
        try:
            shutil.rmtree(tar_dir)
        except OSError as e:
            print("Did not find \"directory\"")
            # print("Error: %s : %s" % (os.file_path, e.strerror))
        return 0

    if opts.clean_all:
        #remove dse output
        fl = []        
        for var in os.listdir():
            if var.endswith(".csv"):
                fl.append(var)
        for var in fl:
            os.remove(var)
        #remove generated files
        try:
            shutil.rmtree(tar_dir)
        except OSError as e:
            print("Did not find \"directory\"")
            # print("Error: %s : %s" % (os.file_path, e.strerror))
        return 0

    #cheak if manual input is given
    inputfiles = []
    if opts.input:
        source_file = opts.input
        inputtop = opts.topfun
        inputpart = opts.part
        template = INPAR.read_template()
    else:
        source_file, inputtop, inputpart, inputfiles, template = INPAR.read_user_input()

    #check if generated_files directory exists
    if not(os.path.exists(tar_dir)):
        os.makedirs(tar_dir)

    #scaleHLS manual optimization
    val = ""
    while val == "":
        val = input("What ScaleHLS optimizations? (Manual / DSE / None)\n")
        if((val == "DSE") or (val == "D") or (val == "d")):
            PYSHLS.scalehls_dse(tar_dir, source_file, inputtop)

            var_forlist, var_arraylist_sized, var_list, var_forlist_scoped, tree_list = INPAR.process_source_file(tar_dir, tar_dir + "/ScaleHLS_DSE_out.cpp", sdse=True)
            #var_forlist = []
            print_variables(var_forlist, var_arraylist_sized)
        elif((val == "Manual") or (val == "M") or (val == "m")):
            opt_knobs, opt_knob_names = PYSHLS.ScaleHLSopt(source_file, inputtop, tar_dir + "/ScaleHLS_opted.c")   
            print_optknobs(opt_knobs, opt_knob_names)

            var_forlist, var_arraylist_sized, var_list, var_forlist_scoped, tree_list = INPAR.process_source_file(tar_dir, tar_dir + "/ScaleHLS_opted.c")
            print_variables(var_forlist, var_arraylist_sized)
        elif((val == "None") or (val == "N") or (val == "n")):
            var_forlist, var_arraylist_sized, var_list, var_forlist_scoped, tree_list = INPAR.process_source_file(tar_dir, source_file)

            #var_forlist, var_arraylist_sized, var_forlist_scoped = INPAR.process_source_file_array('generated_files/ScaleHLS_DSE_out.cpp')
            
            print_variables(var_forlist, var_arraylist_sized)
    
    print("\nScope")
    for i in var_forlist_scoped:
        print(i)

    print("\nTree")
    for item in tree_list:
        item.show()

    # tree_list[0].remove_node(4)

    # print("\nCulledTree")
    # for item in tree_list:
    #     item.show()

    # DPAT.cull_function_by_pattern(tar_dir, tar_dir + "/ML_in.cpp", "1", tree_list[0])

    

    # sortedarray = sortbyhotness(var_forlist_scoped)

    # print("\nSorted Scope")
    # for i in sortedarray:
    #     print(i)

    #create paramfile
    INPAR.create_params(tar_dir, var_forlist, var_arraylist_sized)

    #create template
    INPAR.create_template(tar_dir, source_file, inputfiles, template)

    #Create Random Training Set
    val = ""
    while val == "":
        val = input("Generate Random Training Set? (Y / N)\n")
        # val = "n"
        if((val == "Y") or (val == "y") or (val == "yes")):
            dataset, feature_columns = RT.random_train_RFML(tar_dir, inputtop, inputpart, multiprocess = 4, nub_of_init = 20)
            print(dataset)
        elif((val == "N") or (val == "n") or (val == "no")):
            parameter_file = tar_dir + '/ML_params.csv'
            dataset, feature_columns, label_columns = RT.dataframe_create(parameter_file)
            dataset = pd.read_csv(tar_dir + '/ML_train.csv', index_col=0)
            # dataset = pd.read_csv('generated_files/ML_train(2mm).csv', index_col=0)
            print(dataset)


    DMain.DSE_start(tar_dir, proj_name, dataset, 150, inputtop, inputpart, feature_columns)
    















if __name__ == '__main__':
    main()
