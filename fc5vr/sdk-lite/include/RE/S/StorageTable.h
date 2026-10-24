#pragma once

namespace RE {
    namespace StorageTable {

        struct HashBucket {
            void *unk_0;
            uint16_t bucketIndex;
            uint16_t unk_A;
            uint32_t unk_C;
        };
        static_assert(sizeof(HashBucket) == 0x10);

        struct alignas(64) HashTable {
            uint32_t threadID;
            uint32_t refCount;
            uint64_t unk_8[15];
            uint32_t unk_80; // either last Element or ActiveElement
            uint32_t unk_84;
            uint64_t unk_88;
            uint64_t unk_90;
            void *pParent; // 98 HostOnlyMemory<T,V>
            HashBucket *pBucketsPointer; // A0
            //void* pad_A8;
            uint32_t bucketsSize; //B0 = default 96
            uint32_t unk_B4; // 64 or 16
            uint32_t unk_B8; // 16
            //uint32_t pad_BC;
            HashBucket buckets[0x1000]; // 0xC0 looks like this is initial capacity driven by some logic max size is x4 of initial capacity, for debug purpose will hardcode it to reasonable max
        };
        static_assert(sizeof(HashTable) == 0x1000 * 0x10 + 0xC0);

        template<typename T, typename V>
        struct HostOnlyMemory {
            virtual void unk_0(); // or destructor
            virtual void unk_1();
            virtual void unk_2();
            virtual void unk_3();
            virtual void unk_4();

            V *pTableData;  // pointer equal to unk_18
            HashTable *unk_10; // hashtable size 0x700 aligment 64, could be wrong and this so far looks like array list
            V *tableData; // 0x18  for truct StorageTable::HostOnlyMemory<StorageTable::Camera::Storage,NiCamera * __ptr64,0>: will be pointing to allocated memory with pointers to NiCameras
            void *unk_20;
            uint64_t unk_28; // 0x28
        };

        static_assert(sizeof(HostOnlyMemory<void*,void*>) == 0x30);

        struct alignas(2048 << 2) AsyncColumnRowBuffer {
            uint32_t unk0;
            uint32_t unk4;
            uint8_t unk8[0x3FD * 8 - 8];
            void *unk_1FE8;
            void *unk_1FF0;
            void *unk_1FF8;
        };
        static_assert(sizeof(AsyncColumnRowBuffer) == 0x2000);
        static_assert(offsetof(AsyncColumnRowBuffer, unk_1FE8) == 0x1FE8);

        struct alignas(8) AsyncColumRow {
            uint64_t unk_0;
            uint32_t unk_8;
            AsyncColumnRowBuffer *unk_10;
            AsyncColumnRowBuffer *unk_18;
            uint64_t unk_20;
            uint32_t unk_28;
            uint32_t pad_2C;
            uint32_t unk_30;
            uint64_t unk_38;
            uint64_t unk_40;
            int32_t unk_48;
            uint64_t unk_50;
            uint64_t unk_58;
            int32_t unk_60;
            uint64_t flags_68; // 0x68
        };
        static_assert(sizeof(AsyncColumRow) == 0x70);

        template<typename T, typename V>
        class alignas(64) AsyncTableColumnBase {
        public:
            virtual ~AsyncTableColumnBase() = 0;

            virtual void unk_1();

            virtual void unk_2();

            V* getObject(uint32_t objectId) {
                auto index = indexes[objectId & 0xFFFFFF];
                // static cast to Structure type and offset it by index currently blindly does to pointer 64 bytes
                // objectSize*index+(objectstize-1)
                return &pHostOnlyMemory.pTableData[index];
            }

            uint64_t unk_8;
            uint64_t unk_10;
            uint64_t unk_18;
            uint64_t unk_20;
            uint64_t unk_28;
            uint64_t unk_30;
            uint64_t unk_38;
            uint32_t *indexes;  // objectID & 0xFFFFFF index to pHostOnlyMemory->pTableData
            uint64_t unk_48;
            uint64_t unk_50;
            uint64_t unk_58;
            uint64_t unk_60;
            uint64_t unk_68;
            uint64_t unk_70;
            uint64_t unk_78;
            uint64_t unk_80;
            uint32_t unk_88;
            uint32_t unk_8C;
            uint64_t unk_90;
            uint64_t unk_98;
            uint64_t unk_A0;
            uint64_t unk_A8;
            uint64_t unk_B0;
            uint64_t unk_B8;
            uint64_t offsetRows; // C0
            AsyncColumRow *rows[64]; // 0xC8  will iterate starting from 0xC8 pointers 64 times and validate 0x68 in each pointer
            // looks like valid slot drive which indexes to use either 0x2c8/2d0/40
            uint32_t *unk_indexes0;  // could be memory to objectId mapping
            uint32_t *unk_indexes1;  // 0x2D0 objectId*2 perhaps table for association objectid to objectId pointer to allocated memory (0x200++ size) aligment 8 0x200 points to 2D0
            uint64_t unk_2D8[29];
            HostOnlyMemory<T,V> pHostOnlyMemory; // 3C0 actual offset
            uint64_t unk_400[3]; // pads
        };
        //auto t = check_size<sizeof(AsyncTableColumnBase), 0x440>;
		using AsyncTableColumnBaseVoid = AsyncTableColumnBase<void*, void*>;
		//static_assert(offsetof(AsyncTableColumnBaseVoid), unk_2D8) == 0x2D8);
		//static_assert(offsetof(AsyncTableColumnBaseVoid), rows) == 0xC8);
		//static_assert(offsetof(AsyncTableColumnBaseVoid), pHostOnlyMemory) == 0x3C0);
		static_assert(sizeof(AsyncTableColumnBaseVoid) == 0x440);

        class StorageDataBase {
        public:
            virtual ~StorageDataBase() = 0;

            virtual void unk_1();
            virtual void unk_2();
            virtual void unk_3();
            virtual void unk_4();
            virtual void unk_5();
            virtual void allocateFreeSlotAndSwapBuffers();

            // members
            void *unk_8;
            uint32_t *indexes; // 0x10
            void *unk_18;
            uint64_t unk_20;
            uint64_t unk_28;
            uint64_t unk_30;
            uint64_t unk_38;
            void *unk_40; //pointer arg in constructor
            void *unk_48;
            void *unk_50;
            uint64_t unk_58; // 0x1FAh
            uint64_t unk_60;
            void *unk_68;
            void *unk_70;
            uint64_t unk_78; // 0x1FAh
            uint64_t unk_80;
            uint64_t unk_88;
            uint32_t unk_90[8];
            uint64_t unk_B0;
            uint32_t unk_B8[8];
            void *unk_D8;
            uint32_t unk_E0;

        };

        static_assert(offsetof(StorageDataBase, unk_40) == 0x40);
        static_assert(offsetof(StorageDataBase, unk_E0) == 0xE0);
        static_assert(sizeof(StorageDataBase) == 0xE8);

        template<typename Storage>
        class StorageData : public StorageDataBase {
        public:
            virtual ~StorageData() = 0;
            Storage storageData; // 0xE8
        };

        struct alignas(8) DirectColumnDataStorageInterface {
            virtual void unk_0();
            virtual void unk_1();
            virtual void unk_2();
            virtual void unk_3();
        };

        template <typename T, typename V>
        class WriterDirectStorageData : public DirectColumnDataStorageInterface {
        public:
            HostOnlyMemory<T,V> *pHostOnlyMemory; // 0x8
        };

        static_assert(sizeof(WriterDirectStorageData<void*, void*>) == 0x10);


        template <typename T, typename V>
        class ObserverDirectStorageData : public DirectColumnDataStorageInterface {
		public:
            HostOnlyMemory<T, V> *pHostOnlyMemory; // 0x8
        };;
    }

}
