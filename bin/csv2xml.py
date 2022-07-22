
import csv
import json
import os.path
from   PIL import Image

################################################################################
train_bbox_file        = './tabl/train-annotations-bbox.csv'
validation_bbox_file   = './tabl/validation-annotations-bbox.csv'
test_bbox_file         = './tabl/test-annotations-bbox.csv'

image_sdir             = './images/' # './images/train2017/'
image_ddir             = './images_2/'

__desc__               = {'tin can':0, 'bottle':1};
__desc2_               = {'tin can':'Tin can', 'bottle':'Bottle'};

################################################################################
bbox_files  = [train_bbox_file,  validation_bbox_file,  test_bbox_file ];
idx         = 2;
################################################################################

################################################################################
annotations = {};
images      = [];

with open(bbox_files[idx], 'r') as csvfile :
    csvreader = csv.reader(csvfile, delimiter=',', quotechar='"')
    for row in csvreader :
        if not (row[3] in __desc__) :
            continue;
        # endif
        
        # filename,width,height,class,xmin,ymin,xmax,ymax

        annotation = {    \
            'filename'   : row[0], \
            'width'      : row[1], \
            'height'     : row[2], \
            'label'      : row[3], \
            'x0'         : row[4], \
            'y0'         : row[5], \
            'x1'         : row[6], \
            'y1'         : row[7]  \
        }

        if (row[0] in annotations) :
            annotations[row[0]].append(annotation)
        else :
            images.append(annotation);
            annotations[row[0]] = [annotation]
        # endif
    # endfor
# end-with

cmd = '';

if __name__ == '__main__':
    # Pool(8).map(download, images)

    ################################################################################
    for image in images :

        #######################################
        fullpath = image_sdir + image['filename'];
        if not os.path.isfile(fullpath) :
            continue;
        # endif
        cmd = cmd + 'cp ' + fullpath + ' ' + image_ddir + image['filename'] + '\n';

        filename = image['filename'];

        imgid  = filename.split('.')[0];
        width  = image['width' ];
        height = image['height']; 
        #######################################

        xml_file = open('annotations/' + imgid + '.xml', 'w', encoding="utf-8")
        xml_file.write('<annotation>\n')
        xml_file.write('    <folder>VOC2007</folder>\n')
        xml_file.write('    <filename>' + str(filename) + '</filename>\n')
        xml_file.write('    <size>\n')
        xml_file.write('        <width>'  + str(width ) + '</width>\n')
        xml_file.write('        <height>' + str(height) + '</height>\n')
        xml_file.write('        <depth>3</depth>\n')
        xml_file.write('    </size>\n')
     
        # write the region of image on xml file
        for img_each_label in annotations[filename] :
            xml_file.write('    <object>\n')
            xml_file.write('        <name>' + str(__desc2_[img_each_label['label']]) + '</name>\n')
            xml_file.write('        <pose>Unspecified</pose>\n')
            xml_file.write('        <truncated>0</truncated>\n')
            xml_file.write('        <difficult>0</difficult>\n')
            xml_file.write('        <bndbox>\n')
            xml_file.write('            <xmin>' + str(int(float(img_each_label['x0']))) + '</xmin>\n')
            xml_file.write('            <ymin>' + str(int(float(img_each_label['y0']))) + '</ymin>\n')
            xml_file.write('            <xmax>' + str(int(float(img_each_label['x1']))) + '</xmax>\n')
            xml_file.write('            <ymax>' + str(int(float(img_each_label['y1']))) + '</ymax>\n')
            xml_file.write('        </bndbox>\n')
            xml_file.write('    </object>\n')
     
        xml_file.write('</annotation>')
    ################################################################################

print(cmd)
    
