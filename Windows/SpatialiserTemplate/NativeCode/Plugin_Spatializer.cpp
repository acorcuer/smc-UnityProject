#include "AudioPluginUtil.h"
#include "rayTraceUtil.h"

extern float hrtfSrcData[];
extern float reverbmixbuffer[];

namespace Spatializer
{
    // Variables that need to be shared between instances of the plugin 
    static int numRays = 20;
    static int maxPathLength = 100;
    static int maxNumReflecs = 75;
    const int HRTFLEN = 512;
	const int numBands = 6;
    const float GAINCORRECTION = 2.0f;
    const static int airAbsorbtion[numBands] = {0.002, 0.005, 0.005, 0.007, 0.012, 0.057};
    const static float C = 343.2;
    const static int impLength = std::ceil(44100 * (maxPathLength/C));
    static GeomeTree* triangleTree;
    static raySphere sourceSphere = raySphere(numRays);
    std::vector<float> rayOutputData;
    static bool newTree = true;
    static bool treeInit = false;
    static bool enableDebug;
    
    enum
    {
        P_AUDIOSRCATTN,
        P_FIXEDVOLUME,
        P_CUSTOMFALLOFF,
        P_NUM
    };
    
	class LoudnessAnalyzer
	{
	public:
		float peak[8];
		float rms[8];
		float attack;
		float release;
		float updateperiod;
		float updatecount;
		HistoryBuffer peakbuf;
		HistoryBuffer rmsbuf;
	public:
		void Init(float lengthInSeconds, float updateRateInHz, float attackTime, float releaseTime, float samplerate)
		{
			attack = 1.0f - powf(0.01f, 1.0f / (samplerate * attackTime));
			release = 1.0f - powf(0.01f, 1.0f / (samplerate * releaseTime));
			updateperiod = samplerate / updateRateInHz;
			int length = (int)ceilf(lengthInSeconds * updateRateInHz);
			peakbuf.Init(length);
			rmsbuf.Init(length);
		}

		inline void Feed(const float* inputs, int numchannels)
		{
			float maxPeak = 0.0f, maxRMS = 0.0f;
			for (int i = 0; i < numchannels; i++)
			{
				float x = inputs[i];
				x = fabsf(x);
				peak[i] += (x - peak[i]) * ((x > peak[i]) ? attack : release);
				x *= x;
				rms[i] += (x - rms[i]) * ((x > rms[i]) ? attack : release);
				if (peak[i] > maxPeak)
					maxPeak = peak[i];
				if (rms[i] > maxRMS)
					maxRMS = rms[i];
			}
			if (--updatecount <= 0.0f)
			{
				updatecount += updateperiod;
				peakbuf.Feed(maxPeak);
				rmsbuf.Feed(sqrtf(maxRMS));
			}
		}

		void ReadBuffer(float* buffer, int numsamplesTarget, float windowLength, float samplerate, bool rms)
		{
			int numsamplesSource = (int)ceilf(samplerate * windowLength / updateperiod);
			HistoryBuffer& buf = (rms) ? rmsbuf : peakbuf;
			buf.ReadBuffer(buffer, numsamplesTarget, numsamplesSource, (float)updatecount / (float)updateperiod);
		}
	};


    class HRTFData
    {
        struct CircleCoeffs
        {
            int numangles;
            float* hrtf;
            float* angles;
            
            void GetHRTF(UnityComplexNumber* h, float angle, float mix)
            {
                int index1 = 0;
                while (index1 < numangles && angles[index1] < angle)
                    index1++;
                if (index1 > 0)
                    index1--;
                int index2 = (index1 + 1) % numangles;
                float* hrtf1 = hrtf + HRTFLEN * 4 * index1;
                float* hrtf2 = hrtf + HRTFLEN * 4 * index2;
                float f = (angle - angles[index1]) / (angles[index2] - angles[index1]);
                for (int n = 0; n < HRTFLEN * 2; n++)
                {
                    h[n].re += (hrtf1[0] + (hrtf2[0] - hrtf1[0]) * f - h[n].re) * mix;
                    h[n].im += (hrtf1[1] + (hrtf2[1] - hrtf1[1]) * f - h[n].im) * mix;
                    hrtf1 += 2;
                    hrtf2 += 2;
                }
            }
        };
        
    public:
        CircleCoeffs hrtfChannel[2][14];
        
    public:
        HRTFData()
        {
            float* p = hrtfSrcData;
            for (int c = 0; c < 2; c++)
            {
                for (int e = 0; e < 14; e++)
                {
                    CircleCoeffs& coeffs = hrtfChannel[c][e];
                    coeffs.numangles = (int)(*p++);
                    coeffs.angles = p;
                    p += coeffs.numangles;
                    coeffs.hrtf = new float[coeffs.numangles * HRTFLEN * 4];
                    float* dst = coeffs.hrtf;
                    UnityComplexNumber h[HRTFLEN * 2];
                    for (int a = 0; a < coeffs.numangles; a++)
                    {
                        memset(h, 0, sizeof(h));
                        for (int n = 0; n < HRTFLEN; n++)
                            h[n + HRTFLEN].re = p[n];
                        p += HRTFLEN;
                        FFT::Forward(h, HRTFLEN * 2, false);
                        for (int n = 0; n < HRTFLEN * 2; n++)
                        {
                            *dst++ = h[n].re;
                            *dst++ = h[n].im;
                        }
                    }
                }
            }
        }
    };
    
    static HRTFData sharedData;
    
    struct InstanceChannel
    {
        UnityComplexNumber h[HRTFLEN * 2];
        UnityComplexNumber x[HRTFLEN * 2];
        UnityComplexNumber y[HRTFLEN * 2];
		UnityComplexNumber xh[HRTFLEN * 2];
        float buffer[HRTFLEN * 2];
    };
    
    struct EffectData
    {
        float p[P_NUM];
        Vector3 prevPositons[2];
        std::vector<Ray> sucessfullRays;
		LoudnessAnalyzer momentary;
        InstanceChannel ch[2];
		struct Data
		{
			float p[P_NUM];
			BiquadFilter Octave1[2];
			BiquadFilter Octave2[2];
			BiquadFilter Octave3[2];
			BiquadFilter Octave4[2];
			BiquadFilter Octave5[2];
			BiquadFilter Octave6[2];
			BiquadFilter Octave7[2];
			BiquadFilter Octave8[2];
			BiquadFilter DisplayFilterCoeffs[8];
			float sr;
			Random random;
		};
		union
		{
			Data data;
			unsigned char pad[(sizeof(Data) + 15) & ~15]; // This entire structure must be a multiple of 16 bytes (and and instance 16 byte aligned) for PS3 SPU DMA requirements
		};
    };
    
    inline bool IsHostCompatible(UnityAudioEffectState* state)
    {
        return
        state->structsize >= sizeof(UnityAudioEffectState) &&
        state->hostapiversion >= UNITY_AUDIO_PLUGIN_API_VERSION;
    }
    
    int InternalRegisterEffectDefinition(UnityAudioEffectDefinition& definition)
    {
        int numparams = P_NUM;
        definition.paramdefs = new UnityAudioParameterDefinition[numparams];
        RegisterParameter(definition, "AudioSrc Attn", "", 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, P_AUDIOSRCATTN, "AudioSource distance attenuation");
        RegisterParameter(definition, "Fixed Volume", "", 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, P_FIXEDVOLUME, "Fixed volume amount");
        RegisterParameter(definition, "Custom Falloff", "", 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, P_CUSTOMFALLOFF, "Custom volume falloff amount (logarithmic)");
        definition.flags |= UnityAudioEffectDefinitionFlags_IsSpatializer;
        return numparams;
    }
    
    static UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK DistanceAttenuationCallback(UnityAudioEffectState* state, float distanceIn, float attenuationIn, float* attenuationOut)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        *attenuationOut =
        data->p[P_AUDIOSRCATTN] * attenuationIn +
        data->p[P_FIXEDVOLUME] +
        data->p[P_CUSTOMFALLOFF] * (1.0f / FastMax(1.0f, distanceIn));
        return UNITY_AUDIODSP_OK;
    }
    
    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK CreateCallback(UnityAudioEffectState* state)
    {
        rayOutputData.clear();
        EffectData* effectdata = new EffectData;
        memset(effectdata, 0, sizeof(EffectData));
        state->effectdata = effectdata;
        if (IsHostCompatible(state))
            state->spatializerdata->distanceattenuationcallback = DistanceAttenuationCallback;
        InitParametersFromDefinitions(InternalRegisterEffectDefinition, effectdata->p);
		state->effectdata = effectdata;
		effectdata->momentary.Init(3.0f, (float)state->samplerate, 0.4f, 0.4f, (float)state->samplerate);
        if(enableDebug){
            DebugInUnity(std::string("Spatialiser plugin loaded sucessfully"));
        }
        
        return UNITY_AUDIODSP_OK;
    }
    
    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ReleaseCallback(UnityAudioEffectState* state)
    {
        if(enableDebug){
            DebugInUnity(std::string("Spatailiser plugin released:"));
        }
        EffectData* data = state->GetEffectData<EffectData>();
        delete data;
        return UNITY_AUDIODSP_OK;
    }
    
    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK SetFloatParameterCallback(UnityAudioEffectState* state, int index, float value)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        if (index >= P_NUM)
            return UNITY_AUDIODSP_ERR_UNSUPPORTED;
        data->p[index] = value;
        return UNITY_AUDIODSP_OK;
    }
    
	static void SetupFilterCoeffs(EffectData::Data* data, float samplerate)
	{
		data->Octave1->SetupLowpass(63, samplerate, 1);
		data->Octave2->SetupBandpass(125, samplerate, 1);
		data->Octave3->SetupBandpass(250, samplerate, 1);
		data->Octave4->SetupBandpass(500, samplerate, 1);
		data->Octave5->SetupBandpass(1000, samplerate, 1);
		data->Octave6->SetupBandpass(2000, samplerate, 1);
		data->Octave7->SetupBandpass(4000, samplerate, 1);
		data->Octave8->SetupHighpass(8000, samplerate, 1);
	}


    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK GetFloatParameterCallback(UnityAudioEffectState* state, int index, float* value, char *valuestr)
    {
        EffectData* data = state->GetEffectData<EffectData>();
        if (index >= P_NUM)
            return UNITY_AUDIODSP_ERR_UNSUPPORTED;
        if (value != NULL)
            *value = data->p[index];
        if (valuestr != NULL)
            valuestr[0] = 0;
        return UNITY_AUDIODSP_OK;
    }
    
    int UNITY_AUDIODSP_CALLBACK GetFloatBufferCallback(UnityAudioEffectState* state, const char* name, float* buffer, int numsamples)
    {
#if !UNITY_PS3
		EffectData* effData = state->GetEffectData<EffectData>();
		EffectData::Data* data = &state->GetEffectData<EffectData>()->data;
		//============
	//	effData->momentary.ReadBuffer(buffer, numsamples, 1500, (float)state->samplerate, true);
	//	float* rmsData = effData->momentary.rmsbuf.data;
		/*std::stringstream sstr;
		sstr << "Buffer: ";
		sstr << (rmsData);
		std::string s2 = sstr.str();
		DebugInUnity(std::string(s2));*/

		//=========

		if (strcmp(name, "Coeffs") == 0) {
			SetupFilterCoeffs(data, (float)state->samplerate);
			data->DisplayFilterCoeffs[7].StoreCoeffs(buffer);
			data->DisplayFilterCoeffs[6].StoreCoeffs(buffer);
			data->DisplayFilterCoeffs[5].StoreCoeffs(buffer);
			data->DisplayFilterCoeffs[4].StoreCoeffs(buffer);
			data->DisplayFilterCoeffs[3].StoreCoeffs(buffer);
			data->DisplayFilterCoeffs[2].StoreCoeffs(buffer);
			data->DisplayFilterCoeffs[1].StoreCoeffs(buffer);
			data->DisplayFilterCoeffs[0].StoreCoeffs(buffer);
		}
		/*	else if (strcmp(name, "MomentaryRMS") == 0) {
		effData->momentary.ReadBuffer(buffer, numsamples, data->p[P_Window], (float)state->samplerate, true);
		float* rmsData = effData->momentary.rmsbuf.data;
		}*/
		else
#endif
			memset(buffer, 0, sizeof(float) * numsamples);
		return UNITY_AUDIODSP_OK;
    }
    
    static void GetHRTF(int channel, UnityComplexNumber* h, float azimuth, float elevation)
    {
        float e = FastClip(elevation * 0.1f + 4, 0, 12);
        float f = floorf(e);
        int index1 = (int)f;
        if (index1 < 0)
            index1 = 0;
        else if (index1 > 12)
            index1 = 12;
        int index2 = index1 + 1;
        if (index2 > 12)
            index2 = 12;
        sharedData.hrtfChannel[channel][index1].GetHRTF(h, azimuth, 1.0f);
        sharedData.hrtfChannel[channel][index2].GetHRTF(h, azimuth, e - f);
    }
    
    extern "C" ABA_API void getRayData(long* len, float **data){
        *len = rayOutputData.size();
        auto size = (*len)*sizeof(float);
        *data = static_cast<float*>(CoTaskMemAlloc(size));
        memcpy(*data, rayOutputData.data(), size);
    }
    
    extern "C" ABA_API void debugToggle(bool state){
        enableDebug = state;
    }
    
    extern "C" ABA_API void setTraceParam(int numberOfRays,int maxLen,int maxReflec){
        numRays  = numberOfRays;
        sourceSphere = raySphere(numRays);
        maxPathLength = maxLen;
        maxNumReflecs = maxReflec;
        
    }
    
    extern "C" ABA_API void marshalGeomeTree(int numNodes,int numTri, int depth,int bbl,float boundingBoxes[],int tl,float triangles[],int lsl,int leafSizes[],int tidl, int triangleIds[]) {
        rayOutputData.clear();
        std::deque<float> boundingList(boundingBoxes, boundingBoxes + bbl);
        std::deque<float> triangleList(triangles, triangles + tl);
        std::deque<int> leafSizeList(leafSizes, leafSizes + lsl);
        std::deque<int> triangleIdList(triangleIds, triangleIds + tidl);
        triangleTree = new GeomeTree(depth,&boundingList,&triangleList,&leafSizeList,&triangleIdList);
        if(enableDebug){
            std::stringstream sstr;
            sstr << "received and constructed tree with ";
            sstr << numNodes;
            sstr << " nodes";
            sendStringStream(&sstr);
        }
        newTree = true;
        treeInit = true;
    }
    
    void addToDebugList(Ray* inRay,float* length) {
        rayOutputData.push_back(inRay->origin.X);
        rayOutputData.push_back(inRay->origin.Y);
        rayOutputData.push_back(inRay->origin.Z);
        rayOutputData.push_back(inRay->direction.X);
        rayOutputData.push_back(inRay->direction.Y);
        rayOutputData.push_back(inRay->direction.Z);
        if(length != NULL) {
            rayOutputData.push_back(fabsf(*length));
        }else{
            rayOutputData.push_back(5.0f);
        }
        
    }
    
    void shootRays(std::vector<Ray> *inputRayList,std::vector<Ray> *outputRayList){
        // For each ray in the raylist, itterate backwards to avoid indexing problems when removing
        // from the list
        for (int i = inputRayList->size()-1; i >= 0; i--) {
            // Get list of triangles the ray might intersect with according to bounding heirarchy
            std::deque<Tri> candidates = triangleTree->getCandidates(&inputRayList->at(i));
            // If candidate list has been populated test the ray against each, otherwise delete!
            if(candidates.size() > 0) {
                float min = INFINITY;
                int minDistIdx = 0;
                for(int j = 0; j < candidates.size();j++){
                    float t;
                    bool intersectResult = inputRayList->at(i).testIntersect(&candidates[j], &t);
                    if(intersectResult){
                        if(t < min && t > 0.1f) {
                            min = t;
                            minDistIdx = j;
                        }
                    }
                }
                // if minimum distance is not infinity (therefore no valid intersections)
                // update the ray
                if(min != INFINITY){
                    addToDebugList(&inputRayList->at(i), &min);
                    // Update origin
                    inputRayList->at(i).origin = inputRayList->at(i).origin + (inputRayList->at(i).direction * fabsf(min));
                    // Update number of reflections
                    inputRayList->at(i).numReflecs++;
                    // Update path length
                    inputRayList->at(i).pathLength += fabsf(min);
                    //Update angle
                    inputRayList->at(i).updateDirec(candidates[minDistIdx].faceNorm);
                    if(candidates[minDistIdx].isListener){
                        addToDebugList(&inputRayList->at(i), &min);
                        outputRayList->push_back(inputRayList->at(i));
                        inputRayList->erase(inputRayList->begin()+i);
                    }else{
                        if(inputRayList->at(i).numReflecs >= maxNumReflecs ||inputRayList->at(i).pathLength >= maxPathLength){
                            addToDebugList(&inputRayList->at(i), NULL);
                            inputRayList->erase(inputRayList->begin()+i);
                        }
                    }
                }else {
                    addToDebugList(&inputRayList->at(i), NULL);
                    inputRayList->erase(inputRayList->begin()+i);
                }
            }else {
                addToDebugList(&inputRayList->at(i), NULL);
                inputRayList->erase(inputRayList->begin()+i);
            }
        }
        if(inputRayList->size() > 0) {
            shootRays(inputRayList, outputRayList);
        }
    }
    
    std::vector<float> calcImpResponse(float* listenerMatrix,float* sourceMatrix, float octavePower[],EffectData* data) {
        std::vector<float> impulseResponse(impLength);
        Vector3 sourcePos = Vector3(sourceMatrix[12], sourceMatrix[13], sourceMatrix[14]);
        Vector3 listenerPos = Vector3(listenerMatrix[12], listenerMatrix[13], sourceMatrix[14]);
        bool reShoot = false;
        
        if(sourcePos != data->prevPositons[0]){
            std::stringstream sstr;
            sstr << " CHANGE";
            sendStringStream(&sstr);
            reShoot = true;
            data->prevPositons[0] = sourcePos;
        }
        if(listenerPos != data->prevPositons[1]){
            std::stringstream sstr;
            sstr << " CHANGE";
            sendStringStream(&sstr);
            reShoot = true;
            data->prevPositons[1] = listenerPos;
        }
        
        if(reShoot || newTree ) {
        data->sucessfullRays.clear();
        std::vector<Ray> sourceRays = sourceSphere.getRayList(sourcePos);
        shootRays(&sourceRays,&data->sucessfullRays);
        newTree = false;
            if(enableDebug){
                std::stringstream sstr;
                sstr << data->sucessfullRays.size();
                sstr << " Sucessfull Rays";
                sendStringStream(&sstr);
            }
        }
        
        for(int i = 0; i < 6; i++){
            for(int j = 0; j < data->sucessfullRays.size(); j++) {
                impulseResponse[(int)std::ceil(data->sucessfullRays[j].pathLength/C)] += octavePower[i]*expf(-airAbsorbtion[i]*data->sucessfullRays[j].pathLength)*powf(1-0.5,data->sucessfullRays[j].numReflecs);
            }
        }
        return impulseResponse;
    };
    
    
    UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ProcessCallback(UnityAudioEffectState* state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int outchannels)
    {
		EffectData* data = state->GetEffectData<EffectData>();

        // Check that I/O formats are right and that the host API supports this feature
        if (inchannels != 2 || outchannels != 2 ||
            !IsHostCompatible(state) || state->spatializerdata == NULL)
        {
            memcpy(outbuffer, inbuffer, length * outchannels * sizeof(float));
            return UNITY_AUDIODSP_OK;
        }

		const float* src = inbuffer;
	/*	for (unsigned int n = 0; n < length; n++)
		{
			data->momentary.Feed(src, inchannels);
			src += inchannels;

		}*/
		float o1;
        static const float kRad2Deg = 180.0f / kPI;
        float* m = state->spatializerdata->listenermatrix;
        float* s = state->spatializerdata->sourcematrix;
        
        float px = s[12];
        float py = s[13];
        float pz = s[14];
        float dir_x = m[0] * px + m[4] * py + m[8] * pz + m[12];
        float dir_y = m[1] * px + m[5] * py + m[9] * pz + m[13];
        float dir_z = m[2] * px + m[6] * py + m[10] * pz + m[14];
        
        float azimuth = (fabsf(dir_z) < 0.001f) ? 0.0f : atan2f(dir_x, dir_z);
        if (azimuth < 0.0f)
            azimuth += 2.0f * kPI;
        azimuth = FastClip(azimuth * kRad2Deg, 0.0f, 360.0f);
        
        float elevation = atan2f(dir_y, sqrtf(dir_x * dir_x + dir_z * dir_z) + 0.001f) * kRad2Deg;
        float spatialblend = state->spatializerdata->spatialblend;
        float reverbmix = state->spatializerdata->reverbzonemix;
        
        GetHRTF(0, data->ch[0].h, azimuth, elevation);
        GetHRTF(1, data->ch[1].h, azimuth, elevation);
        float spread = cosf(state->spatializerdata->spread * kPI / 360.0f);
        float spreadmatrix[2] = { 2.0f - spread, spread };
        
        float* reverb = reverbmixbuffer;
        for (int sampleOffset = 0; sampleOffset < length; sampleOffset += HRTFLEN)
        {
			
            for (int c = 0; c < 2; c++)
            {
                float stereopan = 1.0f - ((c == 0) ? FastMax(0.0f, state->spatializerdata->stereopan) : FastMax(0.0f, -state->spatializerdata->stereopan));
                
                InstanceChannel& ch = data->ch[c];
                
                for (int n = 0; n < HRTFLEN; n++)
                {
                    float left  = inbuffer[n * 2];
                    float right = inbuffer[n * 2 + 1];
                    ch.buffer[n] = ch.buffer[n + HRTFLEN];
                    ch.buffer[n + HRTFLEN] = left * spreadmatrix[c] + right * spreadmatrix[1 - c];
                }

                
                UnityComplexNumber windowedInput[HRTFLEN*2];
                
                int startFreq = 250;
                int startIdx = (int)startFreq/((state->samplerate/2)/(HRTFLEN*2));
                float octavePower[numBands];
                
                for (int n = 0; n < HRTFLEN * 2; n++)
                {
                    windowedInput[n].re = (0.54f - 0.46f * cosf(n * (kPI / (float)HRTFLEN*2))) * ch.buffer[n];
                    windowedInput[n].im = 0.0f;
                    ch.x[n].re = ch.buffer[n];
                    ch.x[n].im = 0.0f;
                }
                
                FFT::Forward(windowedInput, HRTFLEN * 2, false);
				UnityComplexNumber impulse[HRTFLEN * 2];
        /*        
                for(int i = 0; i < numBands; i++) {
                    int binStartIdx = startIdx * (i+1);
                    int binWidth = (binStartIdx*2)-binStartIdx;
                    float magSum = 0;
                    for(int j = (binStartIdx-(binWidth/2)); j < (binStartIdx * 2)+(binWidth/2); j++) {
                        magSum += windowedInput[j].Magnitude2()*0.5f*(1.0f-cos(2*kPI*((j/(binWidth*2)))));
                    }
					std::stringstream sstr;
					sstr << " Magnitudes: ";
					sstr << magSum;
					sendStringStream(&sstr);
                    octavePower[i] = sqrtf(magSum*2.0f);
                }
				
                
                if(treeInit){//============
					std::vector<float> imp = calcImpResponse(m, s, octavePower, data);
					for (int n = 0; n < HRTFLEN * 2; n++) {
						impulse[n].re = imp[n];
						impulse[n].im = 0.0f;//===========
					}
				}
                
				FFT::Forward(impulse, HRTFLEN * 2, false);
				for (int n = 0; n < HRTFLEN * 2; n++)
					UnityComplexNumber::Mul<float, float, float>(ch.x[n], ch.h[n], ch.xh[n]);*/

                FFT::Forward(ch.x, HRTFLEN * 2, false);
                
                for (int n = 0; n < HRTFLEN * 2; n++)
                    UnityComplexNumber::Mul<float, float, float>(ch.xh[n], ch.h[n], ch.y[n]);
                
                FFT::Backward(ch.y, HRTFLEN * 2, false);
                
                for (int n = 0; n < HRTFLEN; n++)
                {
                    float s = inbuffer[n * 2 + c] * stereopan;
                    float y = s + (ch.y[n].re * GAINCORRECTION - s) * spatialblend;
					o1 = data->data.Octave1[c].Process(y);
					float o2 = data->data.Octave2[c].Process(y);
					float o3 = data->data.Octave3[c].Process(y);
					float o4 = data->data.Octave4[c].Process(y);
					float o5 = data->data.Octave5[c].Process(y);
					float o6 = data->data.Octave6[c].Process(y);
					float o7 = data->data.Octave7[c].Process(y);
					float o8 = data->data.Octave8[c].Process(y);
					

                    outbuffer[n * 2 + c] = y;
                    reverb[n * 2 + c] += y * reverbmix;
                }
				// ============We have to change this for each octave==============
				const float* src = inbuffer;
				data->momentary.Feed(src, inchannels);
				src += inchannels;
				//======================================

				float rmsData = data->momentary.rms[1];
				std::stringstream sstr1;
				sstr1 << "rms1: ";
				sstr1 << (rmsData);
				std::string s2 = sstr1.str();
				DebugInUnity(std::string(s2));
            }
            
            inbuffer += HRTFLEN * 2;
            outbuffer += HRTFLEN * 2;
            reverb += HRTFLEN * 2;
        }
        
        return UNITY_AUDIODSP_OK;
    }
}
