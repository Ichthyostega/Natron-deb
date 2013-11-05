//  Powiter
//
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 *Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 *contact: immarespond at gmail dot com
 *
 */

#include "Writer.h"

#include <boost/bind.hpp>

#include <QtConcurrentRun>
#include <QtConcurrentMap>
#include <QtCore/QFile>

#include "Global/LibraryBinary.h"
#include "Global/AppManager.h"

#include "Engine/Image.h"
#include "Engine/Settings.h"
#include "Engine/Settings.h"
#include "Engine/Knob.h"
#include "Engine/TimeLine.h"
#include "Engine/Node.h"

#include "Writers/Encoder.h"

using namespace Powiter;
using std::cout; using std::endl;
using std::make_pair;

Writer::Writer(Node* node):
Powiter::OutputEffectInstance(node),
_requestedChannels(Mask_RGBA), // temporary
_premult(false),
_writeOptions(0),
_frameRangeChoosal(0),
_firstFrameKnob(0),
_lastFrameKnob(0)
{
    if(node){
        QObject::connect(getNode()->getApp()->getTimeLine().get(),
                         SIGNAL(frameRangeChanged(int,int)),
                         this,
                         SLOT(onTimelineFrameRangeChanged(int, int)));
        QObject::connect(this,SIGNAL(renderingOnDiskStarted(Writer*,QString,int,int)),getNode()->getApp(),
                         SLOT(onRenderingOnDiskStarted(Writer*, QString, int, int)));
    }
}

Writer::~Writer(){
    if(_writeOptions){
        delete _writeOptions;
    }
}

std::string Writer::className() const {
    return "Writer";
}

std::string Writer::description() const {
    return "The Writer node can render on disk the output of a node graph.";
}

bool Writer::validate() const{
    /*Defaults writing range to readers range, but
     the user may change it through GUI.*/
    if(_filename.empty()){
        return false;
        
    }
    return true;
}

static QString viewToString(int view,int viewsCount){
    if(viewsCount == 1){
        return "";
    }else if(viewsCount == 2){
        if(view == 0){
            return "_left";
        }else{
            return "_right";
        }
    }else{
        return QString("_view")+QString::number(view);
    }
}

boost::shared_ptr<Encoder> Writer::makeEncoder(SequenceTime time,int view,int totalViews,const RectI& rod){
    Powiter::LibraryBinary* binary = Settings::getPowiterCurrentSettings()->_writersSettings.encoderForFiletype(_fileType);
    Encoder* encoder = NULL;
    if(!binary){
        std::string exc("ERROR: Couldn't find an appropriate encoder for filetype: ");
        exc.append(_fileType);
        exc.append(" (");
        exc.append(getName());
        exc.append(")");
        throw std::invalid_argument(exc);
    }else{
        std::pair<bool,WriteBuilder> func = binary->findFunction<WriteBuilder>("BuildWrite");
        if(func.first)
            encoder = func.second(this);
        else{
            throw std::runtime_error("Library failed to create encoder for unknown reason.");
        }
    }
    
    encoder->premultiplyByAlpha(_premult);
    /*check if the filename already contains the extension, otherwise appending it*/
    QString filename(_filename.c_str());
    int i = filename.lastIndexOf(QChar('.'));
    if(i != -1){
        filename.truncate(i+1); // truncate the extension
    }
    filename.append(viewToString(view,totalViews));
    filename.append('.');
    filename.append(_fileType.c_str());
    
    i = filename.lastIndexOf(QChar('#'));
    QString n = QString::number(time);
    if(i != -1){
        filename = filename.replace(i,1,n);
    }else{
        i = filename.lastIndexOf(QChar('.'));
        filename = filename.insert(i, n);
    }
    
    encoder->setOptionalKnobsPtr(_writeOptions);
    encoder->initializeColorSpace();
    Status stat = encoder->_setupFile(filename, rod);
    if(stat == StatFailed){
        return boost::shared_ptr<Encoder>();
    }
    return boost::shared_ptr<Encoder>(encoder);
}

void Writer::initializeKnobs(){
    std::string fileDesc("File");
    _fileKnob = dynamic_cast<OutputFile_Knob*>(appPTR->getKnobFactory().createKnob("OutputFile", this, fileDesc));
    assert(_fileKnob);
    
    std::string renderDesc("Render");
    _renderKnob = dynamic_cast<Button_Knob*>(appPTR->getKnobFactory().createKnob("Button", this, renderDesc));
    assert(_renderKnob);
    
    std::string premultString("Premultiply by alpha");
    Bool_Knob* premult = dynamic_cast<Bool_Knob*>(appPTR->getKnobFactory().createKnob("Bool", this, premultString));
    premult->setValue(_premult);
    
    std::string filetypeStr("File type");
    _filetypeCombo = dynamic_cast<ComboBox_Knob*>(appPTR->getKnobFactory().createKnob("ComboBox", this, filetypeStr));
    const std::map<std::string,Powiter::LibraryBinary*>& _encoders = Settings::getPowiterCurrentSettings()->_writersSettings.getFileTypesMap();
    std::map<std::string,Powiter::LibraryBinary*>::const_iterator it = _encoders.begin();
    for(;it!=_encoders.end();++it) {
        _allFileTypes.push_back(it->first.c_str());
    }
    _filetypeCombo->populate(_allFileTypes);
    
    
    _frameRangeChoosal = dynamic_cast<ComboBox_Knob*>(appPTR->getKnobFactory().createKnob("ComboBox", this, "Frame range"));
    std::vector<std::string> frameRangeChoosalEntries;
    frameRangeChoosalEntries.push_back("Inputs union");
    frameRangeChoosalEntries.push_back("Manual");
    _frameRangeChoosal->populate(frameRangeChoosalEntries);
    
}

Powiter::Status Writer::renderWriter(SequenceTime time){
    const Format& renderFormat = getRenderFormat();
    int viewsCount = getRenderViewsCount();
    //null RoD for key because the key hash computation doesn't take into account the box
    RenderScale scale;
    scale.x = scale.y = 1.;
    /*now that we have our image, we check what is left to render. If the list contains only
     null rect then we already rendered it all*/
    
    RoIMap inputsRoi = getRegionOfInterest(time, scale, renderFormat);
    //inputsRoi only contains 1 element
    RoIMap::const_iterator roi = inputsRoi.begin();
    for(int i = 0 ; i < viewsCount ; ++i){
        boost::shared_ptr<Encoder> encoder;
        try{
            encoder = makeEncoder(time,i,viewsCount,renderFormat);
        }catch(const std::exception& e){
            cout << e.what() << endl;
            return StatFailed;
        }
        if(!encoder){
            return StatFailed;
        }
        boost::shared_ptr<const Powiter::Image> inputImage = roi->first->renderRoI(time, scale,i,roi->second);
        encoder->render(inputImage, i, renderFormat);
        if(!aborted()){
            // finalize file if needed
            encoder->finalizeFile();
        }else{//remove file!
            QFile::remove(encoder->filename());
        }

    }
    //we released the input image and force the cache to clear exceeding entries
    appPTR->clearExceedingEntriesFromNodeCache();
    return StatOK;
}

void Writer::renderFunctor(boost::shared_ptr<const Powiter::Image> inputImage,
                           const RectI& roi,
                           int view,
                           boost::shared_ptr<Encoder> encoder){
    encoder->render(inputImage,view,roi);
}



bool Writer::validInfosForRendering(){
    /*check if filetype is valid*/
    Powiter::LibraryBinary* isValid = Settings::getPowiterCurrentSettings()->_writersSettings.encoderForFiletype(_fileType);
    if(!isValid) {
        return false;
    }
    /*checking if channels are supported*/
    std::pair<bool,WriteBuilder> func = isValid->findFunction<WriteBuilder>("BuildWrite");
    assert(func.second);
    Encoder* write = func.second(this);
    assert(write);
    try {
        write->supportsChannelsForWriting(_requestedChannels);
    } catch (const std::exception &e) {
        cout << "ERROR: " << e.what() << endl;
    }
    delete write;
    
    /*check if write specific knobs have valid values*/
    if (_writeOptions) {
        if (!_writeOptions->allValid()) {
            return false;
        }
    }
    if(_filename.empty()){
        return false;
    }
    if (!input(0)) {
        return false;
    }
    
    return true;
}

void Writer::getFirstFrameAndLastFrame(int* firstFrame,int *lastFrame){
    int index = _frameRangeChoosal->value<int>();
    if(index == 0){
        getFrameRange(firstFrame, lastFrame);
    }else{
        *firstFrame = _firstFrameKnob->value<int>();
        *lastFrame = _lastFrameKnob->value<int>();
    }

}
void Writer::startRendering(){
    
    _fileType = _allFileTypes[_filetypeCombo->value<int>()];
    _filename = _fileKnob->value<QString>().toStdString();
    int firstFrame,lastFrame;
    getFirstFrameAndLastFrame(&firstFrame, &lastFrame);
    if(firstFrame > lastFrame)
        return;
    
    if(validInfosForRendering()){
        getVideoEngine()->refreshTree();
        getVideoEngine()->render(-1,true,false,true,false);
        emit renderingOnDiskStarted(this,_filename.c_str(),firstFrame,lastFrame);
    }
}

void Writer::onKnobValueChanged(Knob* k,Knob::ValueChangedReason /*reason*/){
    if(k == _filetypeCombo){
        int index = _filetypeCombo->value<int>();
        assert(index < (int)_allFileTypes.size());
        _fileType = _allFileTypes[index];
        if(_writeOptions){
            _writeOptions->cleanUpKnobs();
            delete _writeOptions;
            _writeOptions = 0;
        }
        Powiter::LibraryBinary* isValid = Settings::getPowiterCurrentSettings()->_writersSettings.encoderForFiletype(_fileType);
        if(!isValid) return;
        
        QString file(_filename.c_str());
        int pos = file.lastIndexOf(QChar('.'));
        ++pos;
        file.replace(pos, file.size() - pos, _fileType.c_str());
        _fileKnob->setValue(file);
        
        /*checking if channels are supported*/
        std::pair<bool,WriteBuilder> func = isValid->findFunction<WriteBuilder>("BuildWrite");
        if(func.first){
            Encoder* write = func.second(this);
            _writeOptions = write->initSpecificKnobs();
            if(_writeOptions)
                _writeOptions->initKnobs(_fileType);
            delete write;
        }

    }else if(k == _fileKnob){
        _filename = _fileKnob->getValueAsVariant().toString().toStdString();

    }else if(k == _frameRangeChoosal){
        int index = _frameRangeChoosal->value<int>();
        if(index == 0){
            if(_firstFrameKnob){
                delete _firstFrameKnob;
                _firstFrameKnob = 0;
            }
            if(_lastFrameKnob){
                delete _lastFrameKnob;
                _lastFrameKnob = 0;
            }
        }else if(index == 1){
            int first = getApp()->getTimeLine()->firstFrame();
            int last = getApp()->getTimeLine()->lastFrame();
            if(!_firstFrameKnob){
                _firstFrameKnob = dynamic_cast<Int_Knob*>(appPTR->getKnobFactory().createKnob("Int", this, "First frame"));
                _firstFrameKnob->setValue(first);
                _firstFrameKnob->setDisplayMinimum(first);
                _firstFrameKnob->setDisplayMaximum(last);
                _firstFrameKnob->setMinimum(first);
                _firstFrameKnob->setMaximum(last);
                
            }
            if(!_lastFrameKnob){
                _lastFrameKnob = dynamic_cast<Int_Knob*>(appPTR->getKnobFactory().createKnob("Int", this, "Last frame"));
                _lastFrameKnob->setValue(last);
                _lastFrameKnob->setDisplayMinimum(first);
                _lastFrameKnob->setDisplayMaximum(last);
                _lastFrameKnob->setMinimum(first);
                _lastFrameKnob->setMaximum(last);
                
            }
            createKnobDynamically();
        }

    }else if(k == _renderKnob){
        startRendering();
    }
}

void Writer::onTimelineFrameRangeChanged(int f,int l){
    
    if(_firstFrameKnob){
        _firstFrameKnob->setValue(f);
        _firstFrameKnob->setMinimum(f);
        _firstFrameKnob->setDisplayMinimum(f);
        _firstFrameKnob->setDisplayMaximum(l);
        _firstFrameKnob->setMaximum(l);
        
    }
    if(_lastFrameKnob){
        _lastFrameKnob->setValue(l);
        _lastFrameKnob->setDisplayMinimum(f);
        _lastFrameKnob->setDisplayMaximum(l);
        _lastFrameKnob->setMinimum(f);
        _lastFrameKnob->setMaximum(l);
        
    }

}

