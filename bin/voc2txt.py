import xml.etree.ElementTree as ET
from os import getcwd

import argparse
parser = argparse.ArgumentParser()
parser.add_argument("-v", "--verbose", help="increase output verbosity", action="store_true")
parser.add_argument("-s", "--set", type=str, default='coco', help="set selected")
args = parser.parse_args()

sets=[(args.set, 'trainval'), (args.set, 'test')]
wd = getcwd()
classes = ["person"]

def convert_annotation(year, image_id, list_file):
    in_file = open('data/VOCdevkit/%s/Annotations/%s.xml'%(year, image_id))
    tree=ET.parse(in_file)
    root = tree.getroot()

    print('processing ' + image_id)

    nobjs = 0
    lines = '%s/VOCdevkit/%s/JPEGImages/%s.jpg'%(wd, year, image_id)
    for obj in root.iter('object'):
        difficult = obj.find('difficult')
        difficult = 0 if difficult == None else int(difficult.text)

        cls = obj.find('name')
        if cls is None:
        	continue

        cls = cls.text
        if cls not in classes or int(difficult)==1:
            continue

        cls_id = classes.index(cls)
        xmlbox = obj.find('bndbox')
        b = (int(float(xmlbox.find('xmin').text) + 0.5), int(float(xmlbox.find('ymin').text) + 0.5), int(float(xmlbox.find('xmax').text) + 0.5), int(float(xmlbox.find('ymax').text) + 0.5))
        lines += " " + ",".join([str(a) for a in b]) + ',' + str(cls_id)
        nobjs += 1

    if nobjs >= 1:
    	list_file.write(lines)
    	list_file.write('\n')

for year, image_set in sets:
    image_ids = open('data/VOCdevkit/%s/ImageSets/Main/%s.txt'%(year, image_set)).read().strip().split()
    list_file = open('%s_%s.txt'%(year, image_set), 'w')
    for image_id in image_ids:
        convert_annotation(year, image_id, list_file)
    list_file.close()