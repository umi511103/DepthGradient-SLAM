
#include "laserProcessingClass.h"
#include "orbextractor.h"
#include <opencv2/highgui.hpp>
#include <opencv2/opencv.hpp>
#include <sensor_msgs/Image.h>
#include <cv_bridge/cv_bridge.h>
#include <Eigen/Dense>
#include <unordered_set> // 添加這行頭文件  `

//多threads
#include <thread>
#include <vector>
#include <mutex>

#include "opencv2/img_hash.hpp"


//设置特征点提取需要的一些参数
int nFeatures = 10000;//图像金字塔上特征点的数量
int nLevels = 8;//图像金字塔层数
float fScaleFactor = 1.2;//金字塔比例因子
int fIniThFAST = 16; //检测fast角点阈值
int fMinThFAST = 4; //最低阈值

void LaserProcessingClass::init(lidar::Lidar lidar_param_in){
    lidar_param = lidar_param_in;
}


void LaserProcessingClass::downSamplingToMap(const pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_pc_in, pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_pc_out){
    downSizeFilterSurf.setInputCloud(surf_pc_in);
    downSizeFilterSurf.filter(*surf_pc_out);
 
}

//surface===============================

void processImageRegions_surface(const cv::Mat& depthImage, cv::Mat gray, int startY, int endY, int half_window_size,
 double depth_threshold, double gradient_threshold, int intensity_threshold, int window_size, std::vector<cv::Point>& planePixels) {
    for (int y = startY + half_window_size; y < endY - half_window_size; y++) {
        for (int x = half_window_size; x < depthImage.cols - half_window_size; x++) {
            uchar minPixel = 255;
            uchar maxPixel = 0;

            // 先找x軸是否深度相同 再判斷y值的點是否梯度大致相同
            for (int wx = -half_window_size; wx <= half_window_size; ++wx) {
                uchar currentPixel = depthImage.at<uchar>(y, x + wx);
                if (currentPixel != 0) {
                    minPixel = std::min(minPixel, currentPixel);
                    maxPixel = std::max(maxPixel, currentPixel);
                }
            }
            if (maxPixel - minPixel <= depth_threshold) {
                uchar currentPixel_top = depthImage.at<uchar>(y + half_window_size, x);
                uchar currentPixel = depthImage.at<uchar>(y, x);
                uchar currentPixel_down = depthImage.at<uchar>(y - half_window_size, x);

                double gradient_1 = std::abs(currentPixel_top - currentPixel);
                double gradient_2 = std::abs(currentPixel_down - currentPixel);

                if (currentPixel != 0) {
                    double gradient = std::abs(gradient_1 - gradient_2);

                    if (gradient <= gradient_threshold) {
                        planePixels.push_back(cv::Point(x, y));
                        continue;
                    }
                }
            }

            // Reset min and max values
            minPixel = 255;
            maxPixel = 0;

            // Check along Y 軸是否深度相同，再判斷x值的點是否梯度大致相同
            for (int wy = -half_window_size; wy <= half_window_size; ++wy) {
                uchar currentPixel = depthImage.at<uchar>(y + wy, x);
                if (currentPixel != 0) {
                    minPixel = std::min(minPixel, currentPixel);
                    maxPixel = std::max(maxPixel, currentPixel);
                }
            }
            if (maxPixel - minPixel <= depth_threshold) {
                uchar currentPixel_left = depthImage.at<uchar>(y, x + half_window_size);
                uchar currentPixel = depthImage.at<uchar>(y, x);
                uchar currentPixel_right = depthImage.at<uchar>(y, x - half_window_size);

                double gradient_1 = std::abs(currentPixel_left - currentPixel);
                double gradient_2 = std::abs(currentPixel_right - currentPixel);

                if (currentPixel != 0) {
                    double gradient = std::abs(gradient_1 - gradient_2);

                    if (gradient <= gradient_threshold) {
                        planePixels.push_back(cv::Point(x, y));
                        continue;
                    }
                }
            }

            // 影像 
            else {
                                // Check intensity along X axis
                bool is_same_intensity = true;
                uchar reference_intensity = gray.at<uchar>(y, x - half_window_size);
                for (int wx = -half_window_size/3; wx <= half_window_size/3; ++wx) {
                    for (int wy = -half_window_size/3 ; wy <= half_window_size/3 ; ++wy) {
                        if (std::abs(gray.at<uchar>(y + wy, x + wx) - reference_intensity) > intensity_threshold) {
                            is_same_intensity = false;
                            break;
                        }
                    }
                    if (!is_same_intensity) break;
                }

                if (is_same_intensity) {
                    if (depthImage.at<uchar>(y, x) != 0) {
                        planePixels.push_back(cv::Point(x, y));
                        continue;
                    }
                }
            }
        }
    }
}


void processImage_surface(const cv::Mat& depthImage,cv::Mat gray, int half_window_size,
 double depth_threshold, double gradient_threshold , int intensity_threshold, int window_size,
  std::vector<cv::Point>& planePixels) {
    int numThreads = 16; // Number of threads to use
    std::vector<std::thread> threads;
    std::vector<std::vector<cv::Point>> planePixelsList(numThreads);

    // Split the image into regions and create threads
    int totalHeight = depthImage.rows - depthImage.rows / 3.2; // Total height to process
    int heightPerThread = totalHeight / numThreads;
    int remainingHeight = totalHeight % numThreads;
    int startY = depthImage.rows / 3.2; // Start from one quarter of the image
    for (int i = 0; i < numThreads; ++i) {
        int endY = startY + heightPerThread;
        if (i == numThreads - 1) endY += remainingHeight;
        threads.emplace_back(processImageRegions_surface, std::cref(depthImage),gray, startY, endY, half_window_size, depth_threshold,gradient_threshold, intensity_threshold, window_size, std::ref(planePixelsList[i]));
        startY = endY;
    }

    // Join threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Combine results from each thread if needed
    for (const auto& pixels : planePixelsList) {
        planePixels.insert(planePixels.end(), pixels.begin(), pixels.end());
    }
}
//====================================================
//=====================================================

void LaserProcessingClass::pointcloudtodepth(pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_in,
                                             sensor_msgs::ImageConstPtr& image_msg, 
                                             Eigen::Matrix<double, 3, 4>& matrix_3Dto2D,
                                             Eigen::Matrix3d& result,
                                             Eigen::Matrix3d& RR,
                                             Eigen::Vector3d& tt,
                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_first,
                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_out_surf
                                             ){
    cv_bridge::CvImagePtr cv_ptr;
    cv_ptr = cv_bridge::toCvCopy(image_msg, sensor_msgs::image_encodings::BGR8);

    cv::Mat gray;
    cv::cvtColor(cv_ptr->image, gray, cv::COLOR_BGR2GRAY);

    cv::Mat depthImage = cv::Mat::zeros(gray.size(), CV_8UC1);
    cv::Mat depth_store = cv::Mat::zeros(gray.size(), CV_64FC1);

    double scale = (double)87/256;
    // double nani = 0;
    for (int i = 0; i < (int) surf_first->points.size(); i++) {
        if( surf_first->points[i].x > 0){
            double t = surf_first->points[i].x / scale;

            if (t > 255) {
                t = 255;
            }

            Eigen::Vector4d curr_point(surf_first->points[i].x, surf_first->points[i].y, surf_first->points[i].z, 1);
            Eigen::Vector3d curr_point_image = matrix_3Dto2D * curr_point;

            curr_point_image.x() = curr_point_image.x() / curr_point_image.z();
            curr_point_image.y() = curr_point_image.y() / curr_point_image.z();

            int x = static_cast<int>(curr_point_image.x());
            int y = static_cast<int>(curr_point_image.y());
            if (x >= 0 && x < depthImage.cols && y >= 0 && y < depthImage.rows) {
                depthImage.at<uchar>(y, x) = static_cast<uchar>(t);
                depth_store.at<double>(y, x) = curr_point_image.z();
                // nani++;
            }
        }
    }
    std::vector<cv::Point> planePixels;

    //     double otsu_thresh_val = cv::threshold(  //0529
    //     gray, gray, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU
    // );

    cv::Mat blurred , laplacian;
    
    // 使用高斯模糊
    cv::GaussianBlur(gray, gray , cv::Size(3, 3), 1.0);

    // 計算拉普拉斯變換
    // cv::Laplacian(blurred, laplacian, CV_16S, 5);

    // // 轉換為絕對值並轉換為 8 位無符號整數
    // cv::convertScaleAbs(laplacian, gray);

    // blur(gray, gray, cv::Size(3, 3));
    // cv::imshow("after", gray);
    // cv::waitKey(0);

    
    //***********************先看深度值 再看強度值***********************
    int window_size = 7;
    int intensity_threshold = 9; // 假設閾值為10
    int half_window_size = window_size / 2;
    double depth_threshold = 1.1;
    double gradient_threshold = 1.3;

processImage_surface(depthImage,gray, half_window_size, depth_threshold, gradient_threshold, intensity_threshold, window_size, planePixels);




    //***********************先看深度值 再看強度值***********************   

    for (const cv::Point& point : planePixels) {
        int x = point.x;
        int y = point.y;
        double depth_value = depth_store.at<double>(y, x); // 從深度圖像中獲取深度值，注意型態為double
        
        if(depth_value != 0){
            Eigen::Vector3d points_3d(x*depth_value, y*depth_value, depth_value);
            Eigen::Vector3d recover;
            recover = RR.inverse() * (result*points_3d-tt);

            pcl::PointXYZI pcl_point;
            pcl_point.x = recover.x();
            pcl_point.y = recover.y();
            pcl_point.z = recover.z();
            pc_out_surf->push_back(pcl_point);

            // Eigen::Vector4d curr_point(recover.x(), recover.y(), recover.z(), 1);
            // Eigen::Vector3d curr_point_image = matrix_3Dto2D * curr_point;

            // curr_point_image.x() = curr_point_image.x() / curr_point_image.z();
            // curr_point_image.y() = curr_point_image.y() / curr_point_image.z();

            // int xx = static_cast<int>(curr_point_image.x());
            // int yy = static_cast<int>(curr_point_image.y());

            // if (xx >= 0 && xx < depthImage.cols && yy >= 0 && yy < depthImage.rows) {
            //     cv::circle(cv_ptr->image, cv::Point(x, y), 1, cv::Scalar(0, 0, 255), -1);
            // }
        }
    }
    double map_resolution = 0.3;
    downSizeFilterSurf.setLeafSize(map_resolution * 2, map_resolution * 2, map_resolution * 2);
    downSamplingToMap(pc_out_surf, pc_out_surf);
    std::cout << "after plane number = " << pc_out_surf->points.size() << std::endl;

    // for (int i = 0; i < (int)pc_out_surf->points.size(); i++) {
    //     if (pc_out_surf->points[i].x >= 0) {
    //         Eigen::Vector4d curr_point(pc_out_surf->points[i].x, pc_out_surf->points[i].y, pc_out_surf->points[i].z, 1);
    //         Eigen::Vector3d curr_point_image = matrix_3Dto2D * curr_point;

    //         curr_point_image.x() = curr_point_image.x() / curr_point_image.z();
    //         curr_point_image.y() = curr_point_image.y() / curr_point_image.z();

    //         // 檢查投影點是否在邊緣上
    //         int x = static_cast<int>(curr_point_image.x());
    //         int y = static_cast<int>(curr_point_image.y());

    //         // 確保點在圖像範圍內
    //         if (x >= 0 && x < gray.cols && y >= 0 && y < gray.rows) {
    //             cv::circle(cv_ptr->image, cv::Point(x, y), 1, cv::Scalar(0, 0, 255), -1);
    //         }
    //     }
    // }

    // // 顯示檢測到的中心像素值
    // cv::imshow("after plane", cv_ptr->image);
    // cv::waitKey(0);
}

void LaserProcessingClass::featureExtraction(pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_in, 
                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_out_edge, 
                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& surf_first,
                                             sensor_msgs::ImageConstPtr& image_msg, 
                                             Eigen::Matrix<double, 3, 4>& matrix_3Dto2D){

    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(*pc_in, indices);


    int N_SCANS = lidar_param.num_lines;
    std::vector<pcl::PointCloud<pcl::PointXYZI>::Ptr> laserCloudScans;
    for(int i=0;i<N_SCANS;i++){
        laserCloudScans.push_back(pcl::PointCloud<pcl::PointXYZI>::Ptr(new pcl::PointCloud<pcl::PointXYZI>()));
    }

    for (int i = 0; i < (int) pc_in->points.size(); i++)
    {
        // if(pc_in->points[i].x >= 0){
            int scanID=0;
            double distance = sqrt(pc_in->points[i].x * pc_in->points[i].x + pc_in->points[i].y * pc_in->points[i].y);
            if(distance<lidar_param.min_distance || distance>lidar_param.max_distance)
                continue;
            double angle = atan(pc_in->points[i].z / distance) * 180 / M_PI;
            
            // if (N_SCANS == 16)
            // {
            //     scanID = int((angle + 15) / 2 + 0.5);
            //     if (scanID > (N_SCANS - 1) || scanID < 0)
            //     {
            //         continue;
            //     }
            // }
            // else if (N_SCANS == 32)
            // {
            //     scanID = int((angle + 92.0/3.0) * 3.0 / 4.0);
            //     if (scanID > (N_SCANS - 1) || scanID < 0)
            //     {
            //         continue;
            //     }
            // }
            // else if (N_SCANS == 64)
            // {   
                if (angle >= -8.83)
                    scanID = int((2 - angle) * 3.0 + 0.5);
                else
                    scanID = N_SCANS / 2 + int((-8.83 - angle) * 2.0 + 0.5);

                if (angle > 2 || angle < -24.33 || scanID > 63 || scanID < 0)
                {
                    continue;
                }
            // }
            // else
            // {
            //     printf("wrong scan number\n");
            // } // cv::imshow("after", gray);
    // cv::wa
            laserCloudScans[scanID]->push_back(pc_in->points[i]); 
        // }
    }

    pcl::PointCloud<pcl::PointXYZI>::Ptr edge_first(new pcl::PointCloud<pcl::PointXYZI>());
    // pcl::PointCloud<pcl::PointXYZI>::Ptr surf_first(new pcl::PointCloud<pcl::PointXYZI>());

    for(int i = 0; i < N_SCANS; i++){
        if(laserCloudScans[i]->points.size()<131){
            continue;
        }
        // std::cout << "laserCloudScans = " << laserCloudScans[i]->points.size() << std::endl;
        std::vector<Double2d> cloudCurvature; 
        int total_points = laserCloudScans[i]->points.size()-10;
        for(int j = 5; j < (int)laserCloudScans[i]->points.size() - 5; j++){ //一個scan的每個點都算曲率
            double diffX = laserCloudScans[i]->points[j - 5].x + laserCloudScans[i]->points[j - 4].x + laserCloudScans[i]->points[j - 3].x + laserCloudScans[i]->points[j - 2].x + laserCloudScans[i]->points[j - 1].x - 10 * laserCloudScans[i]->points[j].x + laserCloudScans[i]->points[j + 1].x + laserCloudScans[i]->points[j + 2].x + laserCloudScans[i]->points[j + 3].x + laserCloudScans[i]->points[j + 4].x + laserCloudScans[i]->points[j + 5].x;
            double diffY = laserCloudScans[i]->points[j - 5].y + laserCloudScans[i]->points[j - 4].y + laserCloudScans[i]->points[j - 3].y + laserCloudScans[i]->points[j - 2].y + laserCloudScans[i]->points[j - 1].y - 10 * laserCloudScans[i]->points[j].y + laserCloudScans[i]->points[j + 1].y + laserCloudScans[i]->points[j + 2].y + laserCloudScans[i]->points[j + 3].y + laserCloudScans[i]->points[j + 4].y + laserCloudScans[i]->points[j + 5].y;
            double diffZ = laserCloudScans[i]->points[j - 5].z + laserCloudScans[i]->points[j - 4].z + laserCloudScans[i]->points[j - 3].z + laserCloudScans[i]->points[j - 2].z + laserCloudScans[i]->points[j - 1].z - 10 * laserCloudScans[i]->points[j].z + laserCloudScans[i]->points[j + 1].z + laserCloudScans[i]->points[j + 2].z + laserCloudScans[i]->points[j + 3].z + laserCloudScans[i]->points[j + 4].z + laserCloudScans[i]->points[j + 5].z;
            Double2d distance(j,diffX * diffX + diffY * diffY + diffZ * diffZ);
            cloudCurvature.push_back(distance);

        }
        for(int j=0;j<6;j++){
            int sector_length = (int)(total_points/6); //一個scan分成6段
            // std::cout << "sector_length = " << sector_length << std::endl;
            int sector_start = sector_length *j;
            int sector_end = sector_length *(j+1)-1;
            if (j==5){
                sector_end = total_points - 1; 
            }
            std::vector<Double2d> subCloudCurvature(cloudCurvature.begin()+sector_start,cloudCurvature.begin()+sector_end); 
            
            featureExtractionFromSector(laserCloudScans[i],subCloudCurvature, edge_first, surf_first);
            // featureExtractionFromSector(laserCloudScans[i],subCloudCurvature, edge_first);
            
        }

    }

    cv_bridge::CvImagePtr cv_ptr;
    cv_ptr = cv_bridge::toCvCopy(image_msg, sensor_msgs::image_encodings::BGR8);

    cv::Mat gray;
    cv::cvtColor(cv_ptr->image, gray, cv::COLOR_BGR2GRAY);

    // 刪去上三分之一部分
    int third_rows = gray.rows / 4;
    cv::Mat gray_cropped = gray(cv::Rect(0, third_rows, gray.cols, gray.rows - third_rows));

    // 做Canny邊緣檢測
    Canny(gray_cropped, gray_cropped, 150, 100);

    // 用0（黑色）填補回去
    cv::Mat gray_filled(gray.rows, gray.cols, gray.type(), cv::Scalar(0));
    gray_cropped.copyTo(gray_filled(cv::Rect(0, third_rows, gray_cropped.cols, gray_cropped.rows)));
    // cv::imshow("canny",gray_cropped);
    // cv::waitKey(0);


    int window_size = 3; // 可以根据需要调整

    cv_bridge::CvImagePtr cv_ptr_2;
    cv_ptr_2 = cv_bridge::toCvCopy(image_msg, sensor_msgs::image_encodings::BGR8);
    



    // #pragma omp parallel for
    for (int i = 0; i < (int)edge_first->points.size(); i++) {
        if (edge_first->points[i].x >= 0) {
            
            Eigen::Vector4d curr_point(edge_first->points[i].x, edge_first->points[i].y, edge_first->points[i].z, 1);
            Eigen::Vector3d curr_point_image = matrix_3Dto2D * curr_point;

            curr_point_image.x() = curr_point_image.x() / curr_point_image.z();
            curr_point_image.y() = curr_point_image.y() / curr_point_image.z();

            // 检查投影点是否在边缘上
            int x = static_cast<int>(curr_point_image.x());
            int y = static_cast<int>(curr_point_image.y());

            if (x >= 0 && x < gray.cols && y >= 0 && y < gray.rows) {
                // if (gray.at<uchar>(y, x) > 0) {
                    // 检查周围像素是否是边缘
                    bool is_edge_nearby = false;
                    int half_window_size = window_size / 2;
                    for (int dy = -half_window_size; dy <= half_window_size; dy++) {
                        for (int dx = -half_window_size; dx <= half_window_size; dx++) {
                            int nx = x + dx;
                            int ny = y + dy;
                            if (nx >= 0 && nx < gray.cols && ny >= 0 && ny < gray.rows) {
                                if (gray.at<uchar>(ny, nx) > 0) {
                                    is_edge_nearby = true;
                                    break;
                                }
                            }
                        }
                        if (is_edge_nearby) {
                            break;
                        }
                    }

                    if (is_edge_nearby) {
                        cv::circle(cv_ptr_2->image, cv::Point(x, y), 1, cv::Scalar(0, 0, 255), -1);
                        // #pragma omp critical
                        {
                            pc_out_edge->push_back(edge_first->points[i]);
                        }
                        // number++;
                    }
                // }
            }
        }
    }
    double map_resolution = 0.3;
    downSizeFilterSurf.setLeafSize(map_resolution * 2, map_resolution * 2, map_resolution * 2);
    downSamplingToMap(pc_out_edge, pc_out_edge);
    std::cout << "after edge number = " << (int)pc_out_edge->points.size() << std::endl;
    
    // cv::imshow("after edge", cv_ptr_2->image);
    // cv::waitKey(0);

}


void LaserProcessingClass::featureExtractionFromSector(const pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_in, 
                                                             std::vector<Double2d>& cloudCurvature, 
                                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_out_edge ,
                                                             pcl::PointCloud<pcl::PointXYZI>::Ptr& pc_out_surf
                                                             ){

    std::sort(cloudCurvature.begin(), cloudCurvature.end(), [](const Double2d & a, const Double2d & b)
    { 
        return a.value < b.value; 
    }); //由小排到大


    int largestPickedNum = 0;
    std::vector<int> picked_points;
    int point_info_count =0;
    for (int i = cloudCurvature.size()-1; i >= 0; i=i-1)
    {
        int ind = cloudCurvature[i].id; 
        //檢查該點是否已經存在picked_points裡面 若不存在則執行if裡面的事
        if(std::find(picked_points.begin(), picked_points.end(), ind)==picked_points.end()){

            if(cloudCurvature[i].value > 0.1){
                largestPickedNum++;
                picked_points.push_back(ind);
            }

            //一個segment曲率前20大的都push進edge點
            if (largestPickedNum <= 10 && cloudCurvature[i].value > 0.1 && std::abs(pc_in->points[ind].y) >= 0.5){
                pc_out_edge->push_back(pc_in->points[ind]);
                point_info_count++;
            }
            //若超過20個之後的點曲率還是大於5也是push進edge點
            else if(cloudCurvature[i].value > 5 && std::abs(pc_in->points[ind].y) >= 0.5){
                pc_out_edge->push_back(pc_in->points[ind]);
                picked_points.push_back(ind);
            }
            else{
                pc_out_surf->push_back(pc_in->points[ind]);
            }

            // if(std::abs(pc_in->points[i].y) >= 0.5)

        }
    }
}
LaserProcessingClass::LaserProcessingClass(){
    
}

Double2d::Double2d(int id_in, double value_in){
    id = id_in;
    value =value_in;
};

PointsInfo::PointsInfo(int layer_in, double time_in){
    layer = layer_in;
    time = time_in;
};
