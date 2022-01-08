# import generic libraries
import torch
import torch.utils.data
import pandas as pd
import os
import numpy as np
import time
import sys

os.chdir('../')
sys.path.append(os.getcwd())
# import user-written files
import utils.datavisualizer as dv
import data.loader as loader
import models.model_state
import training
import testing
from utils.utils import compute_normalizer
from utils.logger import set_redirects
from utils.utils import save_options

# import options files
import options.model_options as model_params
import options.dataset_options as dynsys_params
import options.train_options as train_params
from models.model_state import ModelState

# %%####################################################################################################################
# Main function
########################################################################################################################


# %% options

# set (high level) options dictionary
options = {
    'dataset': 'narendra_li',  # only use this dynamic system here!
    'model': 'STORN',
    'do_train': True,
    'do_test': True,
    'logdir': 'final',
    'normalize': False,
    'seed': 1234,
    'optim': 'Adam',
    'showfig': False,
    'savefig': True,
    'MCsamples': 30,
    'vary_data': {
        'k_max_train_values': [2000, 5000, 10000, 20000, 30000, 40000, 50000, 60000],
        'k_max_val_values': [5000, 5000, 5000, 5000, 5000, 5000, 5000, 5000],
        'k_max_test_values': [5000, 5000, 5000, 5000, 5000, 5000, 5000, 5000], },
    'optValue': {
        'h_opt': 60,
        'z_opt': 10,
        'n_opt': 1, },
}

addpath = 'test'
# get saving path
path_general = os.getcwd() + '/log/{}/{}/{}/{}/'.format(options['logdir'],
                                                        options['dataset'],
                                                        addpath,
                                                        options['model'], )
# get saving file names
file_name_general = options['dataset']

# %%
if __name__ == "__main__":
    path = path_general + 'data/'
    # check if path exists and create otherwise
    if not os.path.exists(path):
        os.makedirs(path)
    # set logger
    set_redirects(path, file_name_general + '_runlog')

    start_time = time.time()
    print('Run file: final_narendra_li.py')
    print(time.strftime("%c"))

    # get correct computing device
    if torch.cuda.is_available():
        device = torch.device('cuda')
    else:
        device = torch.device('cpu')
    # device = torch.device('cpu')
    print('Device: {}'.format(device))

    # get the options
    options['device'] = device
    options['dataset_options'] = dynsys_params.get_dataset_options(options['dataset'])
    options['model_options'] = model_params.get_model_options(options['model'], options['dataset'],
                                                              options['dataset_options'])
    options['train_options'] = train_params.get_train_options(options['dataset'])
    options['test_options'] = train_params.get_test_options()

    # optimal model parameters
    options['model_options'].h_dim = options['optValue']['h_opt']
    options['model_options'].z_dim = options['optValue']['z_opt']
    options['model_options'].n_layers = options['optValue']['n_opt']

    # print model type and dynamic system type
    print('\n\tModel Type: {}'.format(options['model']))
    print('\tDynamic System: {}\n'.format(options['dataset']))

    # values of evaluation
    k_max_train_values = options['vary_data']['k_max_train_values']
    k_max_val_values = options['vary_data']['k_max_val_values']
    k_max_test_values = options['vary_data']['k_max_test_values']

    # save the options
    save_options(options, path_general, 'options.txt')

    # print number of evaluations
    print('Total number of data point sets: {}'.format(len(k_max_train_values)))

    # allocation
    vaf_all = torch.zeros([options['MCsamples'], len(k_max_train_values)])
    rmse_all = torch.zeros([options['MCsamples'], len(k_max_train_values)])
    likelihood_all = torch.zeros([options['MCsamples'], len(k_max_train_values)])
    df_all = {}

    for mcIter in range(options['MCsamples']):
        print('\n#####################')
        print('MC ITERATION: {}/{}'.format(mcIter + 1, options['MCsamples']))
        print('#####################\n')

        # set the correct device to run on
        options['device'] = device

        for i, _ in enumerate(k_max_train_values):

            # output current choice
            print('\nCurrent run: k_max_train={}\n'.format(k_max_train_values[i]))

            # get current file name
            file_name = file_name_general + '_kmaxtrain_{}_MC{}'.format(k_max_train_values[i], mcIter)

            # select parameters
            kwargs = {"k_max_train": k_max_train_values[i],
                      "k_max_val": k_max_val_values[i],
                      "k_max_test": k_max_test_values[i]}

            # Specifying datasets
            loaders = loader.load_dataset(dataset=options["dataset"],
                                          dataset_options=options["dataset_options"],
                                          train_batch_size=options["train_options"].batch_size,
                                          test_batch_size=options["test_options"].batch_size,
                                          **kwargs)

            # Compute normalizers
            if options["normalize"]:
                normalizer_input, normalizer_output = compute_normalizer(loaders['train'])
            else:
                normalizer_input = normalizer_output = None

            # Define model
            modelstate = ModelState(seed=options["seed"],
                                    nu=loaders["train"].nu, ny=loaders["train"].ny,
                                    model=options["model"],
                                    options=options,
                                    normalizer_input=normalizer_input,
                                    normalizer_output=normalizer_output)
            modelstate.model.to(options['device'])

            # allocation
            df = {}

            if options['do_train']:
                # train the model
                df = training.run_train(modelstate=modelstate,
                                        loader_train=loaders['train'],
                                        loader_valid=loaders['valid'],
                                        options=options,
                                        dataframe=df,
                                        path_general=path_general,
                                        file_name_general=file_name, )

            if options['do_test']:
                # test the model
                df = testing.run_test(options, loaders, df, path_general, file_name)

            # store values
            df_all[mcIter, i] = df

            # save performance values
            vaf_all[mcIter, i] = df['vaf']
            rmse_all[mcIter, i] = df['rmse'][0]
            likelihood_all[mcIter, i] = df['marginal_likeli'].item()

    # %%  save data

    # get saving path
    path = path_general + 'data/'
    # check if path exists and create otherwise
    if not os.path.exists(path):
        os.makedirs(path)

    # filename
    file_name = file_name_general + '.pt'

    # save data
    datasaver = {'df_all': df_all,
                 'vaf_all': vaf_all,
                 'rmse_all': rmse_all,
                 'likelihood_all': likelihood_all}
    # save data
    torch.save(datasaver, path + file_name)

    # plot performance
    dv.plot_perf_ndata(k_max_train_values, vaf_all, rmse_all, likelihood_all, options, path_general)

    # time output
    time_el = time.time() - start_time
    hours = time_el // 3600
    min = time_el // 60 - hours * 60
    sec = time_el - min * 60 - hours * 3600
    print('\nTotal time of file execution: {}:{:2.0f}:{:2.0f} [h:min:sec]'.format(hours, min, sec))
    print(time.strftime("%c"))
