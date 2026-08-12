#ifndef OPENMW_MWPHYSICS_HEIGHTFIELD_H
#define OPENMW_MWPHYSICS_HEIGHTFIELD_H

#include <LinearMath/btScalar.h>

#include <memory>
#include <vector>

class btCollisionObject;
class btHeightfieldTerrainShape;

namespace MWPhysics
{
    class PhysicsTaskScheduler;

    class HeightField
    {
    public:
        HeightField(std::vector<float> heights, int x, int y, int size, int verts, float minH, float maxH,
            PhysicsTaskScheduler* scheduler);
        ~HeightField();

        btCollisionObject* getCollisionObject();
        const btCollisionObject* getCollisionObject() const;
        const btHeightfieldTerrainShape* getShape() const;
        const float* getHeights() const { return mHeights.data(); }
        std::size_t getVertexCount() const { return mVertexCount; }
        float getMinHeight() const { return mMinHeight; }
        float getMaxHeight() const { return mMaxHeight; }

    private:
        std::unique_ptr<btHeightfieldTerrainShape> mShape;
        std::unique_ptr<btCollisionObject> mCollisionObject;
        std::vector<float> mHeights;
#if BT_BULLET_VERSION < 310
        std::vector<btScalar> mBulletHeights;
#endif
        std::size_t mVertexCount;
        float mMinHeight;
        float mMaxHeight;

        PhysicsTaskScheduler* mTaskScheduler;

        void operator=(const HeightField&);
        HeightField(const HeightField&);
    };
}

#endif
