/*************************************************************
*	Implemetation of the multi-person tracking system described in paper
*	"Online Multi-person Tracking by Tracker Hierarchy", Jianming Zhang, 
*	Liliana Lo Presti, Stan Sclaroff, AVSS 2012
*	http://www.cs.bu.edu/groups/ivc/html/paper_view.php?id=268
*
*	Copyright (C) 2012 Jianming Zhang
*
*	This program is free software: you can redistribute it and/or modify
*	it under the terms of the GNU General Public License as published by
*	the Free Software Foundation, either version 3 of the License, or
*	(at your option) any later version.
*
*	This program is distributed in the hope that it will be useful,
*	but WITHOUT ANY WARRANTY; without even the implied warranty of
*	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*	GNU General Public License for more details.
*
*	You should have received a copy of the GNU General Public License
*	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*	If you have problems about this software, please contact: jmzhang@bu.edu
***************************************************************/


#include <cstdio>
#include <iostream>
#include <fstream>

#include "parameter.h"
#include "munkres.h"
#include "multiTrackAssociation.h"
#include "util.h"

using namespace std;

#define HIST_MATCH_THRESH_CONT 0.4//

void WaitingList::update()
{
    for (list<Waiting>::iterator it=w_list.begin();it!=w_list.end();)
    {
        if ((*it).life_count>life_limit)
        {
            w_list.erase(it++);
            continue;
        }
        else
        {
            (*it).life_count++;
        }
        it++;
    }
}
vector<Rect>WaitingList::outputQualified(double thresh)
{
    vector<Rect> ret;
    for (list<Waiting>::iterator it=w_list.begin();it!=w_list.end();)
    {
        if ((*it).accu>thresh)
        {
            ret.push_back((*it).currentWin);
            w_list.erase(it++);
            continue;
        }
        it++;
    }
    return ret;
}
void WaitingList::feed(Rect gt_win,double response)
{
    Point center((int)(gt_win.x+0.5*gt_win.width),(int)(gt_win.y+0.5*gt_win.height));
    for (list<Waiting>::iterator it=w_list.begin();it!=w_list.end();it++)
    {
        double x1=center.x;
        double y1=center.y;
        double x2=(*it).center.x;
        double y2=(*it).center.y;
        double dis=sqrt(pow(x1-x2,2.0)+pow(y1-y2,2.0))*FRAME_RATE;
        double scale_ratio=(*it).currentWin.width/(double)gt_win.width;
        // greedily seek near detection with similar size as the consecutive one
        if (dis<(*it).currentWin.width*2.3 && scale_ratio<1.1 && scale_ratio>0.90) // some consistancy heuristics
        {
            (*it).currentWin=gt_win;
            (*it).center=Point((int)(gt_win.x+0.5*gt_win.width),(int)(gt_win.y+0.5*gt_win.height));
            (*it).accu++;//could be more than 3
            return;
        }
    }
    w_list.push_back(Waiting(gt_win));
}
/************************************************************************/
Controller::Controller(Size sz,int r, int c,double vh,double lr,double thresh_expert)
        :_hit_record(),
         _grid_rows(r),_grid_cols(c),
         _prior_height_variance(vh),
         _frame_size(sz),
         _bodyheight_learning_rate(lr),
         _alpha_hitting_rate(4*TIME_WINDOW_SIZE),_beta_hitting_rate(5),
         waitList((int)TIME_WINDOW_SIZE),
         waitList_suspicious((int)(2*TIME_WINDOW_SIZE)),
         _thresh_for_expert(thresh_expert)
{
    for (int i=0;i<r;i++)
    {
        vector<double> temp1;
        vector<double> temp2;
        for (int j=0;j<c;j++)
        {
            temp1.push_back(-1);//mark
            temp2.push_back(0);

        }
        _bodyheight_map.push_back(temp1);
        _bodyheight_map_count.push_back(temp2);
    }
}
void Controller::takeVoteForHeight(Rect bodysize_win)
{
    double foot_x=bodysize_win.x+0.5*bodysize_win.width;
    double foot_y=bodysize_win.y+bodysize_win.height;
    int r=(int)MIN(foot_y*_grid_rows/_frame_size.height,_grid_rows-1);//rounding
    int c=(int)MIN(foot_x*_grid_cols/_frame_size.width,_grid_cols-1);
    for (int i=-1;i<2;i++)
    {
        for (int j=-1;j<2;j++)
        {
            int r_=MAX(MIN(r+i,_grid_rows-1),0);
            int c_=MAX(MIN(c+j,_grid_cols-1),0);
            if (_bodyheight_map[r_][c_]==-1) // initialize
            {
                _bodyheight_map[r_][c_]=bodysize_win.height;
                _bodyheight_map_count[r_][c_]++;
            }
            else if (_bodyheight_map_count[r][c]>COUNT_NUM)// forgetting history
            {
                _bodyheight_map[r_][c_]=(_bodyheight_learning_rate)*bodysize_win.height+(1-_bodyheight_learning_rate)*_bodyheight_map[r_][c_];
            }
            else // keep all previous memory
            {
                _bodyheight_map_count[r_][c_]++;
                _bodyheight_map[r_][c_]=(1/_bodyheight_map_count[r_][c_])*bodysize_win.height+(1-1/_bodyheight_map_count[r_][c_])*_bodyheight_map[r_][c_];
            }
        }
    }
}
vector<int> Controller::filterDetection(vector<Rect> detction_bodysize)
{
    vector<int> ret;
    for (size_t i=0;i<detction_bodysize.size();i++)
    {
        //filter out those overlap with suspicious area
        bool flag=false;
        for (size_t j=0;j<_suspicious_rect_list.size();j++)
        {
            if (getRectDist(_suspicious_rect_list[j],detction_bodysize[i],OVERLAP)<0.5)//********************important!!
            {
                ret.push_back(BAD);
                flag=true;
                break;
            }
        }
        if (flag)
            continue;

        // filter out bad detections by body height map
        double foot_x=detction_bodysize[i].x+0.5*detction_bodysize[i].width;
        double foot_y=detction_bodysize[i].y+detction_bodysize[i].height;
        int r=(int)MIN(foot_y*_grid_rows/_frame_size.height,_grid_rows-1);//rounding
        int c=(int)MIN(foot_x*_grid_cols/_frame_size.width,_grid_cols-1);
        if (_bodyheight_map[r][c]==-1)
            ret.push_back(NOTSURE);
        else if (abs(_bodyheight_map[r][c]-detction_bodysize[i].height)<(3+10/_bodyheight_map_count[r][c])*sqrt(_prior_height_variance)*_bodyheight_map[r][c])
            ret.push_back(GOOD);
        else
            ret.push_back(BAD);
    }
    return ret;
}
void Controller::takeVoteForAvgHittingRate(list<EnsembleTracker*> _tracker_list)
{
    double vote_count=0;
    vector<double> hitting;
    for (list<EnsembleTracker*>::iterator it=_tracker_list.begin();it!=_tracker_list.end();it++)
    {
        Rect win=(*it)->getBodysizeResult();
        // only moving objects vote
        if ((*it)->getVel()>win.width*0.7)
            _hit_record.recordVote((*it)->getAddNew());
    }
}
void Controller::deleteObsoleteTracker(list<EnsembleTracker*>& _tracker_list)
{
    /*
    Tracker death control. For modifying termination conditions, change here.
    */
    waitList_suspicious.update();
    double l=_hit_record._getAvgHittingRate(_alpha_hitting_rate,_beta_hitting_rate);
    for (list<EnsembleTracker*>::iterator it=_tracker_list.begin();it!=_tracker_list.end();)
    {
        if((*it)->getHitFreq()*TIME_WINDOW_SIZE<=MAX(l-2*sqrt(l),0))
        {
            (*it)->refcDec1();
            (*it)->dump();
            _tracker_list.erase(it++);
            continue;
        }
        else if (!(*it)->getIsNovice() && (*it)->getTemplateNum()<_thresh_for_expert)
        {
            (*it)->demote();
        }
        it++;
    }
}
void Controller::calcSuspiciousArea(list<EnsembleTracker*>& _tracker_list)
{
    double l=_hit_record._getAvgHittingRate(_alpha_hitting_rate,_beta_hitting_rate);
    waitList_suspicious.update();
    for (list<EnsembleTracker*>::iterator it=_tracker_list.begin();it!=_tracker_list.end();)
    {
        if ((*it)->getAddNew() && // has new detection
            (*it)->getHitFreq()*TIME_WINDOW_SIZE<l-sqrt(l) && // low hitting rate
            (*it)->getVel()<(*it)->getBodysizeResult().width*0.14)// no moving
        {
            waitList_suspicious.feed((*it)->getBodysizeResult(),1);
        }
        it++;
    }
    vector<Rect> sus_rects=waitList_suspicious.outputQualified(0.4*TIME_WINDOW_SIZE);
    for (size_t i=0;i<sus_rects.size();i++)
    {
        _suspicious_rect_list.push_back(sus_rects[i]);
    }
}

/************************************************************************/
TrakerManager::TrakerManager(Detector* detector,Mat& frame,double thresh_promotion)
        :_detector(detector),
         _my_char(0),
         _frame_count(0),
         _tracker_count(0),
         resultWriter(RESULT_OUTPUT_XML_FILE),
         _controller(frame.size(),8,8,0.01,1/COUNT_NUM,thresh_promotion)
{

}
TrakerManager::~TrakerManager()
{
    for (list<EnsembleTracker*>::iterator i=_tracker_list.begin();i!=_tracker_list.end();i++)
        delete *i;
}
void TrakerManager::doHungarianAlg(const vector<Rect>& detections)
{
    _controller.waitList.update();

    list<EnsembleTracker*> expert_class;
    list<EnsembleTracker*> novice_class;
    vector<Rect> detection_left;
    for (list<EnsembleTracker*>::iterator it=_tracker_list.begin();it!=_tracker_list.end();it++)
    {
        if ((*it)->getIsNovice())
            novice_class.push_back((*it));
        else
            expert_class.push_back((*it));
    }

    //deal with experts
    int hp_size=expert_class.size();
    int dt_size=detections.size();
    if (dt_size*hp_size>0)
    {
        Matrix<double> matrix(dt_size, hp_size+dt_size);
        vector<bool> indicator;
        for (int i=0;i<dt_size;i++)
        {
            Rect detect_win_GTsize = scaleWin(detections[i],BODYSIZE_TO_DETECTION_RATIO);
            Rect shrinkWin = scaleWin(detections[i],TRACKING_TO_DETECTION_RATIO);
            list<EnsembleTracker*>::iterator j_tl = expert_class.begin();
            for (int j = 0; j < hp_size + dt_size; j++)
            {
                if (j < hp_size)
                {
                    Rect currentWin=(*j_tl)->getResult();
                    double currentWin_cx=currentWin.x+0.5*currentWin.width+0.5;
                    double currentWin_cy=currentWin.y+0.5*currentWin.height+0.5;
                    double detectWin_cx=detections[i].x+0.5*detections[i].width+0.5;
                    double detectWin_cy=detections[i].y+0.5*detections[i].height+0.5;
                    double d=sqrt(pow(currentWin_cx-detectWin_cx,2.0)+pow(currentWin_cy-detectWin_cy,2.0));

                    double ratio = (double)(*j_tl)->getBodysizeResult().width/detect_win_GTsize.width;
                    if (d<(*j_tl)->getAssRadius() /*&& ratio<1.2 && ratio>0.8*/)
                    {
                        double dis_to_last=(*j_tl)->getDisToLast(shrinkWin);

                        /* ad hoc consistence enhancing rule, making association better*/
                        if (dis_to_last/(((double)(*j_tl)->getSuspensionCount()+1)/(FRAME_RATE*5/7)+0.5)<((*j_tl)->getBodysizeResult().width*1.0))
                        {
                            matrix(i,j)=d;//*h;
                        }
                        else
                            matrix(i,j)=INFINITY;
                    }
                    else
                        matrix(i,j)=INFINITY;
                    j_tl++;
                }
                else
                    matrix(i,j)=100000;// dummy
            }
        }
        Munkres m;
        m.solve(matrix);
        for (int i=0;i<dt_size;i++)
        {
            bool flag=false;
            list<EnsembleTracker*>::iterator j_tl=expert_class.begin();
            Rect shrinkWin=scaleWin(detections[i],TRACKING_TO_DETECTION_RATIO);
            for (int j=0;j<hp_size;j++)
            {
                if (matrix(i,j)==0)//matched
                {
                    (*j_tl)->addAppTemplate(_frame_set,shrinkWin);//will change result_temp if demoted
                    flag=true;

                    if ((*j_tl)->getIsNovice())//release the suspension;
                        (*j_tl)->promote();

                    while((*j_tl)->getTemplateNum()>MAX_TEMPLATE_SIZE)
                        (*j_tl)->deletePoorestTemplate();

                    break;
                }
                j_tl++;
            }
            if (!flag )
                detection_left.push_back(detections[i]);
        }
    }
    else
        detection_left=detections;

    //deal with novice class
    dt_size=detection_left.size();
    hp_size=novice_class.size();
    if (dt_size*hp_size>0)
    {
        Matrix<double> matrix(dt_size, hp_size+dt_size);
        for (int i=0;i<dt_size;i++)
        {
            Rect detect_win_GTsize=scaleWin(detection_left[i],BODYSIZE_TO_DETECTION_RATIO);
            Rect shrinkWin=scaleWin(detection_left[i],TRACKING_TO_DETECTION_RATIO);
            list<EnsembleTracker*>::iterator j_tl=novice_class.begin();
            for (int j=0; j<hp_size+dt_size;j++)
            {
                if (j<hp_size)
                {
                    Rect currentWin=(*j_tl)->getResult();
                    double currentWin_cx=currentWin.x+0.5*currentWin.width+0.5;
                    double currentWin_cy=currentWin.y+0.5*currentWin.height+0.5;
                    double detectWin_cx=detection_left[i].x+0.5*detection_left[i].width+0.5;
                    double detectWin_cy=detection_left[i].y+0.5*detection_left[i].height+0.5;
                    double d=sqrt(pow(currentWin_cx-detectWin_cx,2.0)+pow(currentWin_cy-detectWin_cy,2.0));
                    double ratio=(double)(*j_tl)->getBodysizeResult().width/detect_win_GTsize.width;
                    if (d<(*j_tl)->getAssRadius()/* && ratio<1.2 && ratio>0.8*/)
                    {
                        double dis_to_last=(*j_tl)->getDisToLast(shrinkWin);
                        // ad hoc consistence enhancing rule, making association better
                        if (dis_to_last/(((double)(*j_tl)->getSuspensionCount()+1)/(FRAME_RATE*5/7)+0.5)<((*j_tl)->getBodysizeResult().width)*2)
                            matrix(i,j)=d;//************could be changed
                        else
                            matrix(i,j)=INFINITY;
                    }
                    else
                        matrix(i,j)=INFINITY;
                    j_tl++;
                }
                else
                    matrix(i,j)=100000; // dummy
            }
        }
        Munkres m;
        m.solve(matrix);
        for (int i=0;i<dt_size;i++)
        {
            bool flag=false;
            list<EnsembleTracker*>::iterator j_tl=novice_class.begin();
            Rect shrinkWin=scaleWin(detection_left[i],TRACKING_TO_DETECTION_RATIO);
            for (int j=0;j<hp_size;j++)
            {
                if (matrix(i,j)==0)//matched
                {
                    (*j_tl)->addAppTemplate(_frame_set,shrinkWin);//will change result_temp if demoted
                    flag=true;
                    if ((*j_tl)->getIsNovice())//release the suspension
                        (*j_tl)->promote();

                    while((*j_tl)->getTemplateNum()>MAX_TEMPLATE_SIZE)
                        (*j_tl)->deletePoorestTemplate();

                    break;
                }
                j_tl++;
            }
            if (!flag )
                _controller.waitList.feed(scaleWin(detection_left[i],BODYSIZE_TO_DETECTION_RATIO),1.0);
        }
    }
        //starting position
    else if (dt_size>0)
    {
        for (int i=0;i<dt_size;i++)
            _controller.waitList.feed(scaleWin(detection_left[i],BODYSIZE_TO_DETECTION_RATIO),1.0);
    }
}

// Feb 2018 Update: Add Street Crossing Features
void TrakerManager::counterUpdate(PedestrianPosition ancient, PedestrianPosition curt) {
    // 1. Crossing from A_Left to AB
    if (ancient == A_Left && curt == AB) {
        countAB++;
    }
    // 2. Crossing from BC to AB
    if (ancient == BC && curt == AB) {
        countBA++;
    }
        // 3. Crossing from BC to CD
    else if (ancient == BC && curt == CD) {
        countCD++;
    }
        // 4. Crossing from D_Right to CD
    else if (ancient == D_Right && curt == CD) {
        countDC++;
    }
}

PedestrianPosition TrakerManager::getPosition(Point p)
{
    slope_A = 1.0 * (line_A_y1 - line_A_y0) / (line_A_x1 - line_A_x0);
    intercept_A = line_A_y0 - slope_A * line_A_x0;// y = kx + b -> b = y - kx
    slope_B = 1.0 * (line_B_y1 - line_B_y0) / (line_B_x1 - line_B_x0);
    intercept_B = line_B_y0 - slope_B * line_B_x0;
    slope_C = 1.0 * (line_C_y1 - line_C_y0) / (line_C_x1 - line_C_x0);
    intercept_C = line_C_y0 - slope_C * line_C_x0;
    slope_D = 1.0 * (line_D_y1 - line_D_y0) / (line_D_x1 - line_D_x0);
    intercept_D = line_D_y0 - slope_D * line_D_x0;

    // distance_to_line_B = distance between line and points
    distance_to_line_A = (slope_A * p.x - p.y + intercept_A) / sqrt(slope_A * slope_A + 1);
    distance_to_line_B = (slope_B * p.x - p.y + intercept_B) / sqrt(slope_B * slope_B + 1);
    distance_to_line_C = (slope_C * p.x - p.y + intercept_C) / sqrt(slope_C * slope_C + 1);
    distance_to_line_D = (slope_D * p.x - p.y + intercept_D) / sqrt(slope_D * slope_D + 1);

    if (distance_to_line_A < 0){
        return A_Left;
    }
    // Between A, B
    if ((distance_to_line_B <= 0) && (distance_to_line_A >= 0) && p.x >= line_A_x0 && p.x <= line_B_x1){
        return AB;
    }
        // Between B, C
    else if ((distance_to_line_B > 0) && (distance_to_line_C < 0) && p.x >= line_B_x0 && p.x <= line_C_x1){
        return BC;
    }
    else if ((distance_to_line_C >= 0) && (distance_to_line_D <= 0) && p.x >= line_C_x0 && p.x <= line_D_x1){
        return CD;
    }
    else if (distance_to_line_D > 0){
        return D_Right;
    } else {
        return VP_NONE;
    }
}

void TrakerManager::doWork(Mat& frame, int gpu, int frame_n)
{

    std::vector<int> quality;
    quality.push_back(CV_IMWRITE_JPEG_QUALITY);
    quality.push_back(93);

    // For each frame:
    line_A_x0 = 229;
    line_A_y0 = 338;
    line_A_x1 = 663;
    line_A_y1 = 456;
    line_B_x0 = 341;
    line_B_y0 = 326;
    line_B_x1 = 940;
    line_B_y1 = 449;
    line_C_x0 = 408;
    line_C_y0 = 304;
    line_C_x1 = 1021;
    line_C_y1 = 392;
    line_D_x0 = 458;
    line_D_y0 = 295;
    line_D_x1 = 1086;
    line_D_y1 = 373;

    //mask what we don't want
    cv::Size s = frame.size();
    int height = s.height;
    int width = s.width;
//	for (int x = 0; x < width; x++) {
//	    for (int y = 0; y < height; y++) {
//		if((2 * x - 7 * y + 1200> 0) || x + 2 * y < 700 ) {
//		    frame.at<Vec3b>(Point(x, y)) = 0;
//		}
//	    }
//	}

    Mat bgr,hsv,lab;
    frame.copyTo(bgr);
    cvtColor(frame,hsv,CV_RGB2HSV);
    cvtColor(frame,lab,CV_RGB2Lab);
    Mat frame_set[]={bgr,hsv,lab};
    _frame_set = frame_set;

    // resize the input image and detect objects
    _occupancy_map=Mat(frame.rows,frame.cols,CV_8UC1,Scalar(0));
    Mat frame_resize;
    resize(frame,frame_resize,
           Size((int)(frame.cols*HOG_DETECT_FRAME_RATIO),
                (int)(frame.rows*HOG_DETECT_FRAME_RATIO)));
    //	_detector->detect(frame_resize, gpu);// NOTE: the detections are resized into the normal size
    //vector<Rect> detections=_detector->getDetection();
    //vector<double> response=_detector->getResponse();
    vector<int> det_filter;

    //bo: create detect based on the result of YOLO
    vector<Rect> detections;
    vector<double> response;

    int framec;
    ifstream file;
    file.open("../darknet/det.txt");


    while (file.is_open() && file >> framec) {
        int x1, y1, x2, y2, c;
        file >> x1;
        file >> y1;
        file >> x2;
        file >> y2;
        file >> c;
        //cout << "*** " << framec << " " << x1 << " " << y1 << " " << x2 << " " << y2 << endl;
        if (framec == frame_n) {
            Point p1(x1, y1);
            Point p2(x2, y2);
            Rect rec(p1, p2);
            detections.push_back(rec);
        }
        if (framec > frame_n) break;
    }
    //filter the detection
    if (detections.size()>0)
    {
        vector<Rect> detection_bodysize;
        //for (vector<Rect>::iterator it=detections.begin(); it!=detections.end();)
        //{
        //    if (2*it->x-7*it->y+1000>0||it->x+2*it->y<600) {
        //	detections.erase(it);
        //    }
        //    else {
        //	it++;
        //    }
        //}

        for (size_t i=0;i<detections.size();i++)
        {
            detection_bodysize.push_back(scaleWin(detections[i],BODYSIZE_TO_DETECTION_RATIO));
        }
        det_filter=_controller.filterDetection(detection_bodysize);
    }

    vector<Rect> good_detections;
    for (size_t k=0;k<detections.size();k++)
    {
        if (det_filter[k]!=BAD)
            good_detections.push_back(detections[k]);
    }
    // empty the trash bin of removed trackers
    EnsembleTracker::emptyTrash();

    //4,5 termination update matching rate
    _controller.takeVoteForAvgHittingRate(_tracker_list); // calculate the average hitting rate
    _controller.getQualifiedCandidates();
    _controller.deleteObsoleteTracker(_tracker_list);
    _controller.calcSuspiciousArea(_tracker_list);

	// draw detections
	for (size_t it=0;it<detections.size();it++)
	{
		if (det_filter[it]!=BAD)
			rectangle(frame,detections[it],Scalar(0,255,127),2);
		else
			rectangle(frame,detections[it],Scalar(0,255,127),1);
	}

    //for each tracker, do tracking, tracker and template management
    //cout << _tracker_list.size() << endl;
    for (list<EnsembleTracker*>::iterator i=_tracker_list.begin();i!=_tracker_list.end();)
    {
        (*i)->calcConfidenceMap(_frame_set,_occupancy_map);
        (*i)->track(_frame_set,_occupancy_map);
        (*i)->calcScore();
        (*i)->deletePoorTemplate(0.0);

        // update neighbors
        (*i)->updateNeighbors(_tracker_list);

        // moving experts will vote for the body height map
        if (!(*i)->getIsNovice() && (*i)->getVel()>(*i)->getBodysizeResult().width*0.42)
            _controller.takeVoteForHeight((*i)->getBodysizeResult());

        //update occupancy map.
        //Note: demotion is delayed by one frame, so checking template number could help.
        if (!(*i)->getIsNovice() && (*i)->getTemplateNum()>0)
            rectangle(_occupancy_map,(*i)->getResult(),Scalar(1),-1);

        //kill the tracker if it gets out of border
        Rect avgWin=(*i)->getResult();
        if (avgWin.x<=0 ||
            avgWin.x+avgWin.width>=_frame_set[0].cols-1 ||
            avgWin.y<=0 ||
            avgWin.y+avgWin.height>=_frame_set[0].rows-1)
        {
            (*i)->refcDec1();
            (*i)->dump();
            _tracker_list.erase(i++);
            continue;
        }
        i++;
    }

    // do detection association, and promote trackers here
    doHungarianAlg(good_detections);

    //start new trackers
    vector<Rect> qualified=_controller.getQualifiedCandidates();
    for (size_t i=0;i<qualified.size();i++)
    {
        if (_tracker_list.size()<MAX_TRACKER_NUM)
        {
            EnsembleTracker* tracker=new EnsembleTracker(_tracker_count,Size(qualified[i].width,qualified[i].height));
            tracker->refcAdd1();
            Rect iniWin=scaleWin(qualified[i],TRACKING_TO_BODYSIZE_RATIO);
            tracker->addAppTemplate(_frame_set,iniWin);
            _tracker_list.push_back(tracker);
            _tracker_count++;
        }
    }

    int max = 0;

    // register results and draw
    vector<Result2D> output;

    char location[6][12] = {"VP_NONE","A_Left","AB", "BC", "CD", "D_Right"};

    for (list<EnsembleTracker*>::iterator i =_tracker_list.begin(); i !=_tracker_list.end(); i++)
    {
        // Check position for each results!

        (*i)->registerTrackResult();//record the final output!!!
        if (!(*i)->getIsNovice())
        {
            (*i)->updateMatchHist(bgr);
        }
        if ((*i)->getResultHistory().size()>=0)
        {
            //(*i)->drawResult(frame);
            if (!(*i)->getIsNovice() || ((*i)->getIsNovice() && (*i)->compareHisto(bgr,(*i)->getBodysizeResult())>HIST_MATCH_THRESH_CONT))//***************
            {
                (*i)->drawResult(frame, 1 / TRACKING_TO_BODYSIZE_RATIO);

                // These are all about result export
                Rect win = (*i)->getResultHistory().back();
                int id = (*i)->getID();
                Point centroid(win.x + 3, win.y + 3);

                // 1. Insert this Pedestrian's info into curtPosition
                PedestrianPosition curt = getPosition(centroid);
                curtPositions.insert(pair<int, PedestrianPosition >(id, curt));

                // 2. Find this Pedestrian's prevPosition
                std::map<int, PedestrianPosition >::iterator iterPrev;
                iterPrev = prevPositions.find(id);
                std::map<int, PedestrianPosition >::iterator iterEarly;
                iterEarly = earlyPositions.find(id);
                std::map<int, PedestrianPosition >::iterator iterAncient;
                iterAncient = ancientPositions.find(id);

                std::map<int, PedestrianPosition>::iterator iterCrossFrom;
                iterCrossFrom = crossFrom.find(id);
                std::map<int, PedestrianPosition>::iterator iterCrossTo;
                iterCrossTo = crossTo.find(id);

                if (iterAncient != ancientPositions.end()) {
                    PedestrianPosition prev = iterPrev->second;
                    PedestrianPosition early = iterEarly->second;
                    PedestrianPosition ancient = iterAncient->second;
                    bool exitingAB = (ancient == AB && curt == BC) || (ancient == AB && curt == A_Left);
                    bool exitingCD = (ancient == CD && curt == D_Right) || (ancient == CD && curt == BC);
                    // 3. If ancient != curt && this id doesn't have no crossing like that, crossing happened.
                    if (curt != ancient && ancient != VP_NONE && curt != VP_NONE && !exitingAB && !exitingCD) {
                        // No recent crossing record for this id or
                        // If existed, check whether recent crossing is the same as present crossing (double-count)
                        if (iterCrossFrom == crossFrom.end() || (iterCrossFrom->second != ancient && iterCrossFrom->second != curt && iterCrossTo->second != curt && iterCrossTo->second != ancient)) {

                            // 4. Update Crossing List
                            crossFrom.erase(id);
                            crossTo.erase(id);
                            crossFrom.insert(std::pair<int, PedestrianPosition>(id, ancient));
                            crossTo.insert(std::pair<int, PedestrianPosition>(id, curt));

                            // 5. Update Counters
//                            counterUpdate(ancient, curt);
                            if (ancient == A_Left && curt == AB) {
                                countAB++;
                            }
                            // 2. Crossing from BC to AB
                            if (ancient == BC && curt == AB) {
                                countBA++;
                            }
                                // 3. Crossing from BC to CD
                            else if (ancient == BC && curt == CD) {
                                countCD++;
                            }
                                // 4. Crossing from D_Right to CD
                            else if (ancient == D_Right && curt == CD) {
                                countDC++;
                            }
                            
                            cout << "id=" << id << " crossing from " << location[ancient] << " to " << location[curt] << endl;
                            Mat tmp = frame.clone();

                            // 6. Only show circle when it hits the line
                            cv::circle(tmp, centroid, 5, cv::Scalar(255, 255, 255), 5);

                            std::string countABstr = "Cross A -> B: " + std::to_string(countAB);
                            std::string countBAstr = "Cross B -> A: " + std::to_string(countBA);
                            std::string countCDstr = "Cross C -> D: " + std::to_string(countCD);
                            std::string countDCstr = "Cross D -> C: " + std::to_string(countDC);
                            int countTotal = countAB + countBA + countCD + countDC;
                            std::string total = "Total: " + std::to_string(countTotal);
                            cv::putText(tmp, countABstr, cv::Point(5, 75), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 0, 255), 2);
                            cv::putText(tmp, countBAstr, cv::Point(5, 100), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 0, 255), 2);
                            cv::putText(tmp, countCDstr, cv::Point(5, 125), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 0, 255), 2);
                            cv::putText(tmp, countDCstr, cv::Point(5, 150), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 0, 255), 2);
                            cv::putText(tmp, total, cv::Point(5, 175), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 0, 255), 2);
                            std::string address = to_string(countTotal) + "-Frame-" + std::to_string(frame_n) + "-id-" + std::to_string(id) + "-" + location[ancient] + "-" + location[curt] + ".jpg";
                            bool bSuccess = cv::imwrite(address, tmp, quality);
                            if (!bSuccess){
                                std::cout << "Error: Failed to save the image" << std::endl;
                            }
                        }
                    }
                }

                Point tx(win.x + 10, win.y - 10);
                char buff[10];
                sprintf(buff, "%d", id);
                string s = buff;
                //C++: void putText(Mat& img, const string& text, Point org, int fontFace, double fontScale, Scalar color, int thickness=1, int lineType=8, bool bottomLeftOrigin=false)
//				putText(frame, s, tx, FONT_HERSHEY_PLAIN, 2, COLOR((*i)->getID()), 2);
                putText(frame, s, tx, FONT_HERSHEY_COMPLEX_SMALL, 1, cv::Scalar(255, 0, 0), 2);
/*

				//&&&&&
				int count = 0;
				ifstream read("count.txt");
				read >> count;

				int a = atoi(s.c_str()) + 1;
				if (a > count) {
					ofstream myfile ("count.txt");
					count = a;
					cout << "Number of People:" << count << endl;
					myfile << count;
					myfile.close();
				}

				//output result to xml
				double scale=1/TRACKING_TO_BODYSIZE_RATIO-1;
				Size expand_size((int)(scale*win.width+0.5),(int)(scale*win.height+0.5));
				win=win+expand_size-Point((int)(0.5*scale*win.width+0.5),(int)(0.5*scale*win.height+0.5));
				output.push_back(Result2D((*i)->getID(),(float)(win.x+0.5*win.width),(float)(win.y+0.5*win.height),(float)win.width,(float)win.height));
				*/
            }
        }

        // draw matching radius
        //(*i)->drawAssRadius(frame);
    }

    // sort trackers based on number of templates
    _tracker_list.sort(TrakerManager::compareTraGroup);

    // record results to xml file
    resultWriter.putNextFrameResult(output);

    // screen shot
    if (_my_char=='g')
    {
        char buff[20];
        sprintf(buff,"%d.jpg",_frame_count);
        string filename=buff;
        imwrite(filename,frame);
        _my_char=0;
    }

    cv::putText(frame, "A", cv::Point(line_A_x1 + 3, line_A_y1 + 25), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 0, 255), 2);
    cv::putText(frame, "B", cv::Point(line_B_x1 + 3, line_B_y1 + 25), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 0, 255), 2);
    cv::putText(frame, "C", cv::Point(line_C_x1 + 3, line_C_y1 + 25), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 0, 255), 2);
    cv::putText(frame, "D", cv::Point(line_D_x1 + 3, line_D_y1 + 25), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 0, 255), 2);
    // Line A
    line(frame, Point(229, 338), Point(663, 456), Scalar(0, 0, 255), 2, 8);
    // Line B
    line(frame, Point(341, 326), Point(940, 449), Scalar(0, 0, 255), 2, 8);
    // Line C
    line(frame, Point(408, 304), Point(1021, 392), Scalar(0, 0, 255), 2, 8);
    // Line D
    line(frame, Point(458, 295), Point(1086, 373), Scalar(0, 0, 255), 2, 8);

    std::string countABstr = "Cross A -> B: " + std::to_string(countAB);
    std::string countBAstr = "Cross B -> A: " + std::to_string(countBA);
    std::string countCDstr = "Cross C -> D: " + std::to_string(countCD);
    std::string countDCstr = "Cross D -> C: " + std::to_string(countDC);
    int countTotal = countAB + countBA + countCD + countDC;
    std::string total = "Total: " + std::to_string(countTotal);
    cv::putText(frame, countABstr, cv::Point(5, 75), cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(0, 0, 255), 2);
    cv::putText(frame, countBAstr, cv::Point(5, 100), cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(0, 0, 255), 2);
    cv::putText(frame, countCDstr, cv::Point(5, 125), cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(0, 0, 255), 2);
    cv::putText(frame, countDCstr, cv::Point(5, 150), cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(0, 0, 255), 2);
    cv::putText(frame, total, cv::Point(5, 175), cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(0, 0, 255), 2);

    // Execute deep copy from curtPosition to prevPosition
    ancientPositions = earlyPositions;
    earlyPositions = prevPositions;
    prevPositions = curtPositions;
    // Clear curtPosition
    curtPositions.clear();

    _frame_count++;
}
