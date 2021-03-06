#
# Copyright (c) 2018 cTuning foundation.
# See CK COPYRIGHT.txt for copyright details.
#
# SPDX-License-Identifier: BSD-3-Clause.
# See CK LICENSE.txt for licensing details.
#

import os
import re
import json
import shutil
import numpy as np
import scipy.io
from scipy.ndimage import zoom

def recreate_dir(d):
  if os.path.isdir(d):
    shutil.rmtree(d)
  os.mkdir(d)

def ck_preprocess(i):
  print('\n--------------------------------')
  def my_env(var): return i['env'][var]
  def dep_env(dep, var): return i['deps'][dep]['dict']['env'][var]

  # Init variables from environment
  BATCH_COUNT = int(my_env('CK_BATCH_COUNT'))
  BATCH_SIZE = int(my_env('CK_BATCH_SIZE'))
  IMAGES_COUNT = BATCH_COUNT * BATCH_SIZE
  SKIP_IMAGES = int(my_env('CK_SKIP_IMAGES'))
  IMAGE_DIR = dep_env('imagenet-val', 'CK_ENV_DATASET_IMAGENET_VAL')
  IMAGE_SIZE = int(dep_env('weights', 'CK_ENV_MOBILENET_RESOLUTION'))
  MODE_SUFFIX = '-{}-{}-{}'.format(IMAGE_SIZE, BATCH_SIZE, BATCH_COUNT)
  IMAGE_LIST = my_env('CK_IMAGE_LIST') + MODE_SUFFIX + '.txt'
  BATCHES_DIR = my_env('CK_BATCHES_DIR') + MODE_SUFFIX
  BATCH_LIST = my_env('CK_BATCH_LIST') + MODE_SUFFIX + '.txt'
  RESULTS_DIR = my_env('CK_RESULTS_DIR')
  PREPARE_ALWAYS = my_env('CK_PREPARE_ALWAYS')
  IMAGE_FILE = my_env('CK_IMAGE_FILE')

  # Single file mode
  if IMAGE_FILE:
    assert os.path.isfile(IMAGE_FILE)
    PREPARE_ALWAYS = 'YES'
    BATCH_COUNT = 1
    BATCH_SIZE = 1
    IMAGES_COUNT = 1
    SKIP_IMAGES = 0
    IMAGE_DIR, IMAGE_FILE = os.path.split(IMAGE_FILE)
    print('Single file mode')
    print('Image file: {}'.format(IMAGE_FILE))

  print('Batch size: {}'.format(BATCH_SIZE))
  print('Batch count: {}'.format(BATCH_COUNT))
  print('Batch list: {}'.format(BATCH_LIST))
  print('Skip images: {}'.format(SKIP_IMAGES))
  print('Image dir: {}'.format(IMAGE_DIR))
  print('Image list: {}'.format(IMAGE_LIST))
  print('Image size: {}'.format(IMAGE_SIZE))
  print('Batches dir: {}'.format(BATCHES_DIR))
  print('Results dir: {}'.format(RESULTS_DIR))


  def prepare_batches():
    print('\nPrepare images...')

    # Load processing image filenames
    images = []
    if IMAGE_FILE:
      # Single file mode
      images.append(IMAGE_FILE)
    else:
      # Directory mode
      assert os.path.isdir(IMAGE_DIR), 'Input dir does not exit'
      files = [f for f in os.listdir(IMAGE_DIR) if os.path.isfile(os.path.join(IMAGE_DIR, f))]
      files = [f for f in files if re.search(r'\.jpg$', f, re.IGNORECASE)
                                or re.search(r'\.jpeg$', f, re.IGNORECASE)]
      assert len(files) > 0, 'Input dir does not contain image files'
      files = sorted(files)[SKIP_IMAGES:]
      assert len(files) > 0, 'Input dir does not contain more files'
      images = files[:IMAGES_COUNT]
      if len(images) < IMAGES_COUNT:
        for _ in range(IMAGES_COUNT-len(images)):
          images.append(images[-1])

    # Save image list file
    assert IMAGE_LIST, 'Image list file name is not set'
    with open(IMAGE_LIST, 'w') as f:
      for img in images:
        f.write('{}\n'.format(img))

    dst_images = []

    for img_file in images:
      src_img_path = os.path.join(IMAGE_DIR, img_file)
      dst_img_path = os.path.join(BATCHES_DIR, img_file) + '.npy'

      img = scipy.misc.imread(src_img_path)
      # check if grayscale and convert to RGB
      if len(img.shape) == 2:
        img = np.dstack((img,img,img))
      # drop alpha-channel if present
      if img.shape[2] > 3:
        img = img[:,:,:3]

      # The same image preprocessing steps are used for MobileNet as for Inception:
      # https://github.com/tensorflow/models/blob/master/research/slim/preprocessing/inception_preprocessing.py

      # Crop the central region of the image with an area containing 87.5% of the original image.
      new_w = int(img.shape[0] * 0.875)
      new_h = int(img.shape[1] * 0.875)
      offset_w = (img.shape[0] - new_w)/2
      offset_h = (img.shape[1] - new_h)/2
      img = img[offset_w:new_w+offset_w, offset_h:new_h+offset_h, :]

      # Zoom to target size
      zoom_w = float(IMAGE_SIZE)/float(img.shape[0])
      zoom_h = float(IMAGE_SIZE)/float(img.shape[1])
      img = zoom(img, [zoom_w, zoom_h, 1])

      # Each image is a batch in NCHW format
      img = img.transpose(2, 0, 1)
      img = np.expand_dims(img, 0)
      img = np.ascontiguousarray(img)

      np.save(dst_img_path, img)
      dst_images.append(dst_img_path)

      if len(dst_images) % 10 == 0:
        print('Prepared images: {} of {}'.format(len(dst_images), len(images)))

    # Save image list file
    assert BATCH_LIST, 'Batch list file name is not set'
    with open(BATCH_LIST, 'w') as f:
      for img in dst_images:
        f.write('{}\n'.format(img))

  # Prepare results directory
  recreate_dir(RESULTS_DIR)


  # Prepare batches or use prepared
  do_prepare_batches = True
  if PREPARE_ALWAYS != 'YES':
    do_prepare_batches = False

  if not do_prepare_batches:
    if not os.path.isdir(BATCHES_DIR):
      do_prepare_batches = True

  if do_prepare_batches:
    recreate_dir(BATCHES_DIR)
    prepare_batches()
  else:
    print('\nBatches preparation is skipped, use previous batches')

  print('--------------------------------\n')
  return {'return': 0}

