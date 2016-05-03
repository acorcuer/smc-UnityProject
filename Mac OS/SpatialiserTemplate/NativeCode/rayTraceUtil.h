#ifndef rayTraceUtil_h
#define rayTraceUtil_h
#include <vector>
#include <deque>
#include <string>
#include <sstream>
#include <utility>


#if UNITY_WIN
#define ABA_API __declspec(dllexport)
#else
#define ABA_API
#endif

typedef void(*DebugCallback) (const char *str);
DebugCallback gDebugCallback;

extern "C" ABA_API void RegisterDebugCallback (DebugCallback callback)
{
    if (callback)
    {
        gDebugCallback = callback;
    }
}

void DebugInUnity(std::string message)
{
    if (gDebugCallback)
    {
        gDebugCallback(message.c_str());
    }
}


class Vector3 {
public:
    float X, Y, Z;
    inline Vector3(){};
    inline Vector3(float n_x, float n_y, float n_z)
    { X = n_x; Y = n_y; Z = n_z;}
    inline void setPos(float n_x, float n_y, float n_z)
    {X = n_x; Y = n_y; Z = n_z;}
    inline Vector3 operator + (const Vector3& A) const
    {return Vector3(X + A.X, Y + A.Y, Z + A.Z); }
    inline float Dot( const Vector3& A ) const
    { return A.X*X + A.Y*Y + A.Z*Z; }
    inline float operator [] (const int& A) const{
        switch(A){
        case 0:
        return X;
        break;
        case 1:
        return Y;
        break;
        case 2:
        return Z;
        break;
        default:
        return NULL;
        }
    }
};

class Tri {
public:
    Vector3 P1,P2,P3,faceNorm;
    inline Tri(){};
    inline Tri(Vector3 n_p1,Vector3 n_p2,Vector3 n_p3,Vector3 n_fn)
    {P1 = n_p1; P2 = n_p2; P3 = n_p3; faceNorm = n_fn;}
    inline void setVerts(Vector3 n_p1,Vector3 n_p2,Vector3 n_p3)
    {P1 = n_p1; P2 = n_p2; P3 = n_p3;}
    inline void setFaceNorm(Vector3 n_fn)
    {faceNorm = n_fn;}
};

class Ray {
public:
    Vector3 origin,direction;
    float pathLength = 0;
    inline Ray(){};
    inline Ray(Vector3 n_o, Vector3 n_d)
    {origin = n_o;direction = n_d;}
    inline void setOrigin(Vector3 n_o)
    {origin = n_o;}
    inline void setDirection(Vector3 n_d)
    {direction = n_d;}
    inline void addLength(float add)
    {pathLength += add;}
    
};

class Bounds {
public:
    Vector3 max,min;
    inline Bounds() {}
    inline Bounds(Vector3 n_max,Vector3 n_min){
        max = n_max;
        min = n_min;
    }
    inline void setBB(Vector3 n_max,Vector3 n_min){
        max = n_max;
        min = n_min;
    }
    inline bool testIntersect(Ray *r, float tmin, float tmax) {
        for(int i = 0; i < 3; i++){
            float invD = 1.0f / r->direction[i];
            float t0 = (min[i] - r->origin[i]) * invD;
            float t1 = (max[i] - r->origin[i]) * invD;
            if(invD < 0.0f) {
                std::swap(t0, t1);
            }
            tmin = t0 > tmin ? t0 : tmin;
            tmax = t1 > tmax ? t1 : tmin;
            if(tmax < tmin){
                return false;
            }
        }
        return true;
    }
};

class Node {
private:
    Bounds boundingBox;
    int depth;
    std::vector<Tri> nodeTriangles;
    bool isLeaf = false;
public:
    Node *leftChild,*rightChild;
    inline Node() {}
    inline Node(int parentDepth, int maxDepth, std::deque<float> *boundingBoxArray,std::deque<float> *triangleArray,std::deque<int> *leafSizeArray) {
        this->depth = parentDepth+1;
        this->boundingBox = Bounds(Vector3(boundingBoxArray->at(0),boundingBoxArray->at(1),boundingBoxArray->at(2)),Vector3(boundingBoxArray->at(3),boundingBoxArray->at(4),boundingBoxArray->at(5)));
        boundingBoxArray->erase(boundingBoxArray->begin(), boundingBoxArray->begin()+6);
        if(this->depth >= maxDepth-1) {
            this->isLeaf = true;
            int sizeOfLeaf = leafSizeArray->front();
            leafSizeArray->pop_front();
            nodeTriangles.resize(sizeOfLeaf);
            for (int i=0; i < sizeOfLeaf; i++) {
                Vector3 P1 = Vector3(triangleArray->at(0),triangleArray->at(1),triangleArray->at(2));
                Vector3 P2 = Vector3(triangleArray->at(3),triangleArray->at(4),triangleArray->at(5));
                Vector3 P3 = Vector3(triangleArray->at(6),triangleArray->at(7),triangleArray->at(8));
                Vector3 faceNorm = Vector3(triangleArray->at(9),triangleArray->at(10),triangleArray->at(11));
                triangleArray->erase(triangleArray->begin(), triangleArray->begin()+12);
                nodeTriangles[i] = *new Tri(P1,P2,P3,faceNorm);
            }
        }else{
            leftChild = new Node(depth,maxDepth,boundingBoxArray,triangleArray,leafSizeArray);
            rightChild = new Node(depth,maxDepth,boundingBoxArray,triangleArray,leafSizeArray);
        }
    }
    inline bool intersects(Ray *testRay){
        return boundingBox.testIntersect(testRay, 0, 10);
    }
    inline bool getIsLeaf() {
        return isLeaf;
    }
    inline std::vector<Tri>* getTri() {
        return &nodeTriangles;
    }
    inline int numTri() {
        return nodeTriangles.size();
    }
};

class GeomeTree {
    public:
    Node masterNode;
    inline GeomeTree() {};
    inline GeomeTree(int maxDepth,std::deque<float> *boundingBoxArray,std::deque<float> *triangleArray,std::deque<int> *leafSizeArray) {
        masterNode = *new Node(1,maxDepth,boundingBoxArray,triangleArray,leafSizeArray);
    };
    
    inline std::deque<Tri> getCandidates(Ray *testRay) {
        std::deque<Tri> candidates;
        collision(&masterNode, testRay, &candidates);
        return candidates;
    }
    
    inline void collision(Node *currentNode, Ray *testRay, std::deque<Tri> *candidates) {
        if(currentNode->intersects(testRay)) {
            if (currentNode->getIsLeaf()) {
                if(currentNode->numTri() != 0) {
                    for(int i = 0; i < currentNode->getTri()->size();i++){
                        candidates->push_back(currentNode->getTri()->at(i));
                    }
                    
                }

            }else {
                collision(currentNode->leftChild, testRay, candidates);
                collision(currentNode->rightChild, testRay, candidates);
        
        }
    }
}
};



#endif /* rayTraceUtil_h */
