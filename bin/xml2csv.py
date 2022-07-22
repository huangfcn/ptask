import os
import glob
import pandas as pd
import xml.etree.ElementTree as ET


image_path = '/home/fenghuang/workspace/dataset/VOCdevkit/VOC2012/Annotations/'

def xml_to_csv(path):
    # print(path)

    xml_list = []
    for xml_file in glob.glob(path + '/*.xml'):
        print(xml_file)
        tree = ET.parse(xml_file)
        root = tree.getroot()
        for member in root.findall('object'):
            value = (root.find('filename').text,
                     int(root.find('size').find('width').text),
                     int(root.find('size').find('height').text),
                     member.find('name').text,
                     int(float(member.find('bndbox').find('xmin').text)+0.5),
                     int(float(member.find('bndbox').find('ymin').text)+0.5),
                     int(float(member.find('bndbox').find('xmax').text)+0.5),
                     int(float(member.find('bndbox').find('ymax').text)+0.5)
                     )
            xml_list.append(value)
    column_name = ['filename', 'width', 'height', 'class', 'xmin', 'ymin', 'xmax', 'ymax']
    xml_df = pd.DataFrame(xml_list, columns=column_name)
    return xml_df


def main():
    # image_path = os.path.join(rootpath, subdir)
    # print(glob.glob(image_path+'*.xml'))
    xml_df = xml_to_csv(image_path)
    xml_df.to_csv('raccoon_labels.csv', index=None)
    print('Successfully converted xml to csv.')


main()